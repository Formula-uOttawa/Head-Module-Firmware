/* tcan.c - TCAN2410-Q1 SBC driver on SPI1 (CM7), hardened port of the hub module driver (see tcan.h). */
#include "tcan.h"
#include "main.h"

extern SPI_HandleTypeDef hspi1;

#define TCAN_BUFFER_SIZE 2
#define TCAN_WD_DISABLE_TRIES 5 // attempts at switching the watchdog off
#define TCAN_SPI_WRITE_BIT 1u   // bit 0 of the SPI address byte: 1 = write, 0 = read (the register number sits in bits 7..1)
#define TCAN_SPI_TIMEOUT_MS 10U // a 2 byte frame at 2.5 MHz takes ~7 us; anything near this means a dead bus
static uint8_t tcan_tx_buffer[TCAN_BUFFER_SIZE];
static uint8_t tcan_rx_buffer[TCAN_BUFFER_SIZE];
uint8_t tcan_global_int;

// Not implemented: the SBC interrupt output (CAN_INT pin) is not used; the interrupt flag registers are only read/cleared at boot.

uint8_t tcan_init_status;
bool tcan_eeprom_wd_loaded;
bool tcan_init_ok; // TCAN2410Init() succeeded: watchdog off, normal mode, CAN transceiver on
int tcan_temp_save_result = -1;
uint8_t tcan_int_snapshot[4]; // TCAN_REG_INT_GLOBAL, TCAN_REG_INT_1, TCAN_REG_INT_2, TCAN_REG_INT_3 as found at boot, before they are cleared

// The SBC starts in standby with its watchdog running and turns the 3V3 rail off if it is not serviced in time, so
// the first thing to do after reset is to switch the watchdog off, and to verify that it took.
// A scratch pad round trip comes first: a dead link (MISO stuck low) would otherwise read back as "watchdog disabled".
static bool tcan_scratch_test(void) {
    uint8_t test = 0;
    if (!(TCANSpiWrite(TCAN_REG_SCRATCH_PAD_SPI, 0xA5) && TCANSpiRead(TCAN_REG_SCRATCH_PAD_SPI, &test) && test == 0xA5)) return false;
    test = 0;
    return TCANSpiWrite(TCAN_REG_SCRATCH_PAD_SPI, 0x5A) && TCANSpiRead(TCAN_REG_SCRATCH_PAD_SPI, &test) && test == 0x5A;
}

// True if the SBC is already running with the watchdog off (a warm MCU reset while the SBC kept its state).
static bool tcan_watchdog_already_off(void) {
    uint8_t w1, w2;
    if (!TCANSpiRead(TCAN_REG_WD_CONFIG_1, &w1) || !TCANSpiRead(TCAN_REG_WD_CONFIG_2, &w2)) return false;
    return (w1 >> 6) == 0 && (w2 & WD_STBY_DIS);
}

static bool tcan_disable_watchdog(void) {
    uint8_t v;
    if (!tcan_scratch_test()) return false;
    // After a debugger/software reset the SBC is usually already configured; do not rewrite its registers then
    // (config registers may be protected in normal mode, and every write is a chance to upset it).
    if (tcan_watchdog_already_off()) {
        tcan_init_status |= TCAN_INIT_WARM_START;
        return true;
    }
    // TCAN_REG_WD_CONFIG_1: watchdog config (bits 7-6) = 00 disabled
    if (!TCANSpiWrite(TCAN_REG_WD_CONFIG_1, WD_DISABLED)) return false;
    // TCAN_REG_WD_CONFIG_2 bit 0 (WD_STBY_DIS) = 1: no watchdog in standby either (read-modify-write, other bits keep their value).
    // Best effort: if the register is protected this does not stop the start-up, it is only reported in tcan_init_status.
    if (TCANSpiRead(TCAN_REG_WD_CONFIG_2, &v)) {
        (void)TCANSpiWrite(TCAN_REG_WD_CONFIG_2, v | WD_STBY_DIS);
        if (!TCANSpiRead(TCAN_REG_WD_CONFIG_2, &v) || !(v & WD_STBY_DIS)) tcan_init_status |= TCAN_INIT_STBY_NOT_DISABLED;
    }
    if (!TCANSpiRead(TCAN_REG_WD_CONFIG_1, &v)) return false;
    return (v >> 6) == 0;
}

static bool tcan_init_once(void);

bool TCAN2410Init(void) {
    bool ok = tcan_init_once();
    if (!ok) tcan_init_status |= TCAN_INIT_FAILED;
    return ok;
}

static bool tcan_init_once(void) {
    tcan_init_status = 0;
    tcan_init_ok = false;

    // nCS must be high before the first frame so that the SBC sees a falling edge that starts it
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
    HAL_Delay(1);

    // disable the watchdog; retry quickly, the standby watchdog gives us well under a second
    bool wd_ok = false;
    for (int attempt = 0; attempt < TCAN_WD_DISABLE_TRIES && !wd_ok; attempt++) {
        wd_ok = tcan_disable_watchdog();
        if (!wd_ok) {
            tcan_init_status |= TCAN_INIT_WD_RETRIED;
            HAL_Delay(2);
        }
    }
    if (!wd_ok) {
        tcan_init_status |= TCAN_INIT_WD_FAILED;
        return false;
    }

    // keep what the SBC reports (why it restarted, watchdog/fault flags) for the boot message, then clear it
    (void)TCANSpiRead(TCAN_REG_INT_GLOBAL, &tcan_int_snapshot[0]);
    (void)TCANSpiRead(TCAN_REG_INT_1, &tcan_int_snapshot[1]);
    (void)TCANSpiRead(TCAN_REG_INT_2, &tcan_int_snapshot[2]);
    (void)TCANSpiRead(TCAN_REG_INT_3, &tcan_int_snapshot[3]);

    // power-on flag set and watchdog already off at boot: the settings came from the SBC's EEPROM
    tcan_eeprom_wd_loaded = (tcan_init_status & TCAN_INIT_WARM_START) && (tcan_int_snapshot[2] & 0x40);

    // Clear power-on and fail-safe interrupt flags (only the ones that are set; a failed clear is not fatal)
    if (tcan_int_snapshot[1]) (void)TCANSpiWrite(TCAN_REG_INT_1, tcan_int_snapshot[1]);
    if (tcan_int_snapshot[2]) (void)TCANSpiWrite(TCAN_REG_INT_2, tcan_int_snapshot[2]);
    if (tcan_int_snapshot[0]) (void)TCANSpiWrite(TCAN_REG_INT_GLOBAL, tcan_int_snapshot[0]);

#if HEAD_TEMP_SAVE_WD_EEPROM
    // one-shot: persist the watchdog-off configuration while the SBC is still in standby (config registers unlocked)
    tcan_temp_save_result = (int)TCANSaveWatchdogConfig(false);
#endif

    // set sbc into normal mode (skipped when it is already configured that way)
    uint8_t sbc_cfg = 0;
    if (!(TCANSpiRead(TCAN_REG_SBC_CONFIG, &sbc_cfg) && sbc_cfg == SBC_STARTUP_CFG)) {
        if (!TCANSpiWrite(TCAN_REG_SBC_CONFIG, SBC_STARTUP_CFG)) {
            tcan_init_status |= TCAN_INIT_MODE_FAILED;
            return false;
        }
    }

    // Switch the CAN transceiver on: it resets to "wake capable" (driver Z/Z), only 110b / 111b drive the bus in normal mode
    uint8_t can_cntrl = 0;
    if (!TCANSpiWrite(TCAN_REG_CAN_CNTRL_1, CAN_CNTRL_1_ON_NORMAL) || !TCANSpiRead(TCAN_REG_CAN_CNTRL_1, &can_cntrl) ||
        (can_cntrl & CAN_CNTRL_1_TRX_MASK) != CAN_CNTRL_1_ON_NORMAL) {
        tcan_init_status |= TCAN_INIT_CAN_FAILED;
        return false;
    }

    // the transmitter needs its 5 V VCAN supply; without it frames never reach the bus (reported, not fatal)
    uint8_t vcan = 0;
    if (!TCANSpiRead(TCAN_REG_HSS_CNTL3, &vcan) || !(vcan & VCAN_STATUS_BIT)) tcan_init_status |= TCAN_INIT_NO_VCAN;

    tcan_init_ok = true;
    return true;
}

// One 2 byte SPI frame with a finite timeout; on failure the transfer is aborted and chip select released.
static bool tcan_xfer(uint8_t reg, uint8_t val, bool write, uint8_t *read_val) {
    tcan_tx_buffer[0] = (uint8_t)(reg << 1 | (write ? TCAN_SPI_WRITE_BIT : 0u));
    tcan_tx_buffer[1] = write ? val : 0;

    tcan_rx_buffer[0] = 0;
    tcan_rx_buffer[1] = 0;

    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi1, tcan_tx_buffer, tcan_rx_buffer, TCAN_BUFFER_SIZE, TCAN_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);

    if (status != HAL_OK) {
        (void)HAL_SPI_Abort(&hspi1);
        return false;
    }

    tcan_global_int = tcan_rx_buffer[0];
    if (!write) *read_val = tcan_rx_buffer[1];

    // TODO: implement CRC
    return true;
}

bool TCANSpiWrite(uint8_t reg, uint8_t val) {
    return tcan_xfer(reg, val, true, NULL);
}

bool TCANSpiRead(uint8_t reg, uint8_t *val) {
    return tcan_xfer(reg, 0, false, val);
}

/* ---- saving the watchdog configuration to the SBC's EEPROM (see TCAN2410-Q1 datasheet 8.3.14, Table 8-25) ---------- */

// CRC-8 as used by the SBC's SPI CRC with the default polynomial: poly 0x2F, init 0xFF, no reflection, final XOR 0xFF
static uint8_t tcan_crc8(const uint8_t *d, size_t n) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x2F) : (uint8_t)(crc << 1);
    }
    return (uint8_t)(crc ^ 0xFF);
}

// One 3-byte transaction while SPI CRC is enabled in the SBC: address+R/W, data (filler 00 for a read), CRC of those two.
static bool tcan_xfer_crc(uint8_t reg, bool write, uint8_t data, uint8_t *read_val) {
    uint8_t tx[3], rx[3] = { 0, 0, 0 };
    tx[0] = (uint8_t)(reg << 1 | (write ? TCAN_SPI_WRITE_BIT : 0u));
    tx[1] = write ? data : 0;
    tx[2] = tcan_crc8(tx, 2);
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
    HAL_StatusTypeDef st = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 3, TCAN_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
    if (st != HAL_OK) {
        (void)HAL_SPI_Abort(&hspi1);
        return false;
    }
    tcan_global_int = rx[0];
    if (read_val) *read_val = rx[1];
    return true;
}

// Switch SPI CRC off again (a CRC-framed write), then confirm with a normal read. Retried: leaving CRC on breaks SPI.
static bool tcan_crc_off(void) {
    for (int i = 0; i < 5; i++) {
        uint8_t v = 0xFF;
        (void)tcan_xfer_crc(TCAN_REG_CRC_CNTL, true, 0x00, NULL);
        if (TCANSpiRead(TCAN_REG_CRC_CNTL, &v) && v == 0x00) return true;
        HAL_Delay(2);
    }
    return false;
}

TCanSaveStatus_t TCANSaveWatchdogConfig(bool force) {
    uint8_t w1 = 0xFF, w2 = 0, vc = 0, v = 0;

    if (!tcan_scratch_test()) return TCAN_SAVE_NO_LINK;
    // what gets stored is whatever the registers hold right now, so they must hold "watchdog off"
    if (!TCANSpiRead(TCAN_REG_WD_CONFIG_1, &w1) || !TCANSpiRead(TCAN_REG_WD_CONFIG_2, &w2) || (w1 >> 6) != 0 || !(w2 & WD_STBY_DIS))
        return TCAN_SAVE_WD_NOT_OFF;
    if (!TCANSpiRead(TCAN_REG_HSS_CNTL3, &vc) || !(vc & VCAN_STATUS_BIT)) return TCAN_SAVE_NO_VCAN;
    if (tcan_eeprom_wd_loaded && !force) return TCAN_SAVE_ALREADY_SAVED;

    // clear the SPI/CRC error flags so that errors during the save are attributable to it
    (void)TCANSpiWrite(TCAN_REG_INT_3, 0x90);
    (void)TCANSpiWrite(TCAN_REG_INT_4_REG, 0x02);

    // 1. enable SPI CRC (this write is still an ordinary 2-byte frame) and confirm with a CRC-framed read
    (void)TCANSpiWrite(TCAN_REG_CRC_CNTL, 0x01);
    if (!tcan_xfer_crc(TCAN_REG_CRC_CNTL, false, 0, &v) || v != 0x01) {
        return tcan_crc_off() ? TCAN_SAVE_CRC_FAILED : TCAN_SAVE_CRC_STUCK;
    }

    // 2. save: EEPROM_SAVE (bit 7) + access code 0xA, with CRC; then wait for EEPROM_SAVE to self-clear
    bool sent = tcan_xfer_crc(TCAN_REG_EEPROM_CTRL, true, 0x8A, NULL);
    bool done = false;
    uint32_t t0 = HAL_GetTick();
    while (sent && HAL_GetTick() - t0 < 500) {
        if (tcan_xfer_crc(TCAN_REG_EEPROM_CTRL, false, 0, &v) && !(v & 0x80)) { done = true; break; }
        HAL_Delay(2);
    }

    // 3. always switch CRC off again
    if (!tcan_crc_off()) return TCAN_SAVE_CRC_STUCK;
    if (!sent) return TCAN_SAVE_NOT_ACCEPTED;
    if (!done) return TCAN_SAVE_TIMEOUT;

    // 4. the save frame must not have been rejected (SPIERR/CRCERR in TCAN_REG_INT_3)
    if (!TCANSpiRead(TCAN_REG_INT_3, &v) || (v & 0x90)) return TCAN_SAVE_NOT_ACCEPTED;

    // 5. ask the SBC to check the EEPROM CRC (EEPROM_CRC_CHK) and make sure it did not complain
    (void)TCANSpiWrite(TCAN_REG_EEPROM_CTRL, 0x40);
    HAL_Delay(5);
    if (!TCANSpiRead(TCAN_REG_INT_4_REG, &v) || (v & 0x02)) return TCAN_SAVE_VERIFY_FAILED;
    return TCAN_SAVE_OK;
}
