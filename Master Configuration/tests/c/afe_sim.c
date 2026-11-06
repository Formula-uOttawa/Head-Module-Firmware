/* Host simulation of the hub AFE driver: real afe_ctrl.c + live_slots.c + the hub blob given as argv[1] (real hub_cfg.c), against a
 * stub HAL with a small ADS124S08 model on the SPI bus. DRDY edges and DMA completions are driven by the test. */
#include <stdio.h>
#include <string.h>
#include "afe_ctrl.h"
#include "debug_sniff.h"
#include "hub_store.h"
#include "live_slots.h"
#include "main.h"
#include "sensor_state.h"

GPIO_TypeDef stub_gpioa, stub_gpioc, stub_gpiod;
SPI_TypeDef stub_spi2;
static SPI_HandleTypeDef hspi2_storage;
SPI_HandleTypeDef hspi2;

static HubConfig_t g_cfg;
const HubConfig_t *HubCfg_Active(void) { return &g_cfg; }

static uint32_t g_tick = 5000;
static int g_fail = 0;
static int cs_low_a, cs_low_b;      /* chip select levels: 1 = asserted (pin low) */
static char g_log[4096];             /* event log: "RST0 RST1 A:RREG B:WREG A:START ..." */
static uint8_t regs[2][0x12];
static uint8_t dev_id[2] = { 0x00, 0x00 };
static int dma_result = HAL_OK;      /* what HAL_SPI_TransmitReceive_DMA returns */
static int dma_active, dma_afe;
static uint8_t *dma_tx, *dma_rx;
static uint16_t dma_n;
static int abort_calls, dma_starts;
static int b_dead;                   /* AFE B does not drive DOUT: reads as zeros, ignores writes */
static int corrupt_b_reg4;           /* AFE B stores a wrong value in register 4 (read-back mismatch) */
static int sniff_last_tx_id = -1, sniff_last_tx_len, sniff_last_rx_len, sniff_events;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void logf_(const char *s) { strcat(g_log, s); strcat(g_log, " "); }

uint32_t HAL_GetTick(void) { return g_tick; }
void HAL_Delay(uint32_t ms) { g_tick += ms; }
void HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t pin, GPIO_PinState s)
{
    if (p == CS_A_GPIO_Port && pin == CS_A_Pin) cs_low_a = (s == GPIO_PIN_RESET);
    else if (p == CS_B_GPIO_Port && pin == CS_B_Pin) cs_low_b = (s == GPIO_PIN_RESET);
    else if (p == RST_AB_GPIO_Port && pin == RST_AB_Pin) logf_(s == GPIO_PIN_RESET ? "RST0" : "RST1");
}
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t pin) { (void)p; (void)pin; return GPIO_PIN_RESET; }

void debug_sniff_push(SniffEvent_t e, uint8_t id, const uint8_t *d, uint8_t len)
{
    (void)d;
    sniff_events++;
    if (e == SNIFF_AFE_TX) { sniff_last_tx_id = id; sniff_last_tx_len = len; }
    if (e == SNIFF_AFE_RX) sniff_last_rx_len = len;
}

static int selected_afe(void)
{
    if (cs_low_a && !cs_low_b) return 0;
    if (cs_low_b && !cs_low_a) return 1;
    return -1;
}

/* the ADS124S08 model: apply the bytes of one transaction (CS low .. high) */
static void model_apply(int afe, const uint8_t *tx, uint8_t *rx, uint16_t n, bool apply_wreg)
{
    char tag[32];
    const char *name = afe ? "B" : "A";
    memset(rx, 0, n);
    if (afe == 1 && b_dead) return;
    if (n == 1) {
        const char *c = tx[0] == 0x08 ? "START" : tx[0] == 0x0A ? "STOP" : tx[0] == 0x06 ? "RESET" : "CMD";
        snprintf(tag, sizeof tag, "%s:%s", name, c);
        logf_(tag);
        return;
    }
    if ((tx[0] & 0xE0) == 0x20) {
        uint8_t reg = tx[0] & 0x1F, cnt = (uint8_t)(tx[1] + 1);
        for (uint8_t k = 0; k < cnt && 2 + k < n; k++) rx[2 + k] = (reg + k == 0) ? dev_id[afe] : regs[afe][reg + k];
        snprintf(tag, sizeof tag, "%s:RREG", name);
        logf_(tag);
    } else if ((tx[0] & 0xE0) == 0x40 && apply_wreg) {
        uint8_t reg = tx[0] & 0x1F, cnt = (uint8_t)(tx[1] + 1);
        for (uint8_t k = 0; k < cnt && 2 + k < n; k++) regs[afe][reg + k] = (afe == 1 && corrupt_b_reg4 && reg + k == 4) ? (uint8_t)(tx[2 + k] ^ 0x01) : tx[2 + k];
        snprintf(tag, sizeof tag, "%s:WREG", name);
        logf_(tag);
    }
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t timeout)
{
    (void)h; (void)timeout;
    int afe = selected_afe();
    CHECK(afe >= 0, "polled transfer needs exactly one chip select asserted");
    CHECK(!dma_active, "polled transfer while a DMA transfer is running");
    if (afe < 0) return HAL_ERROR;
    model_apply(afe, tx, rx, n, true);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n)
{
    (void)h;
    if (dma_result != HAL_OK) return (HAL_StatusTypeDef)dma_result;
    int afe = selected_afe();
    CHECK(afe >= 0, "DMA transfer needs exactly one chip select asserted");
    CHECK(!dma_active, "second DMA transfer started while one is running");
    dma_active = 1; dma_afe = afe; dma_tx = tx; dma_rx = rx; dma_n = n; dma_starts++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *h) { (void)h; abort_calls++; dma_active = 0; return HAL_OK; }

/* finish the running DMA transfer: the AFE shifts out `code` (24 bit) while taking the WREG */
static void dma_complete(int32_t code)
{
    CHECK(dma_active, "no DMA transfer running");
    if (!dma_active) return;
    uint8_t tmp[16];
    model_apply(dma_afe, dma_tx, tmp, dma_n, true);
    dma_rx[0] = (uint8_t)(code >> 16); dma_rx[1] = (uint8_t)(code >> 8); dma_rx[2] = (uint8_t)code;
    dma_active = 0;
    HAL_SPI_TxRxCpltCallback(&hspi2);
}

static void drdy(int afe) { HAL_GPIO_EXTI_Callback(afe ? DR_B_Pin : DR_A_Pin); }

static void reset_world(void)
{
    memset(g_log, 0, sizeof g_log);
    memset(regs, 0, sizeof regs);
    regs[0][1] = regs[1][1] = 0x80;
    regs[0][9] = regs[1][9] = 0x10;
    dev_id[0] = dev_id[1] = 0;
    corrupt_b_reg4 = 0; b_dead = 0;
    dma_result = HAL_OK; dma_active = 0; abort_calls = 0; dma_starts = 0; cs_low_a = cs_low_b = 0;
    hspi2.Instance = SPI2;
    hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase = SPI_PHASE_2EDGE;
    LiveSlots_Init();
}


int main(int argc, char **argv)
{
    (void)hspi2_storage;
    uint8_t blob[HUB_MAX_BLOB];
    FILE *bf = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!bf) { printf("usage: afe_sim <hub blob>\n"); return 2; }
    size_t blen = fread(blob, 1, sizeof blob, bf);
    fclose(bf);
    if (HubCfg_Parse(blob, (uint32_t)blen, &g_cfg) != HUB_CFG_OK) { printf("blob rejected\n"); return 2; }
    const AfeStats_t *st = AfeGetStats();

    /* ---- init ---------------------------------------------------------------------------------------------------- */
    reset_world();
    CHECK(AfeInit(), "AfeInit failed");
    CHECK(st->status == 0x3F, "status 0x%02X (both ID ok, both running, both configs ok)", st->status);
    CHECK(strstr(g_log, "RST0 RST1") == g_log, "reset pulse first: %s", g_log);
    const char *start_a = strstr(g_log, "A:START"), *start_b = strstr(g_log, "B:START");
    CHECK(start_a && start_b, "START for both AFEs");
    CHECK(start_a && strstr(g_log, "A:WREG") && strstr(g_log, "A:WREG") < start_a, "channel 0 loaded before START");
    CHECK(!cs_low_a && !cs_low_b, "chip selects idle high after init");
    CHECK(regs[0][2] == 0x9C && regs[0][4] == 0x1D && regs[0][7] == 0xFF, "A channel 0 regs %02X %02X %02X", regs[0][2], regs[0][4], regs[0][7]);
    CHECK(regs[1][2] == 0x7C && regs[1][4] == 0x17 && regs[1][5] == 0x01, "B channel 0 regs %02X %02X %02X", regs[1][2], regs[1][4], regs[1][5]);
    CHECK(LiveSlots_Flags(0) == LIVE_FLAG_STALE && LiveSlots_Flags(6) == LIVE_FLAG_STALE, "registered, not sampled yet = stale only");
    CHECK(LiveSlots_Flags(11) == (LIVE_FLAG_STALE | LIVE_FLAG_UNCONFIGURED), "unmapped slot");

    /* ---- one DRDY on A: CS_A (not CS_B), value sign extended, next channel loaded --------------------------------- */
    drdy(0);
    CHECK(dma_active && dma_afe == 0 && cs_low_a && !cs_low_b, "A DRDY must assert CS_A only");
    CHECK(dma_tx[0] == 0x42 && dma_tx[1] == 6 && dma_n == 9, "WREG header %02X %02X n=%u", dma_tx[0], dma_tx[1], dma_n);
    CHECK(dma_tx[2] == 0x7C && dma_tx[3] == 0x00 && dma_tx[4] == 0x1D, "next channel (1) regs sent: %02X %02X %02X", dma_tx[2], dma_tx[3], dma_tx[4]);
    CHECK(sniff_last_tx_id == 0 && sniff_last_tx_len == 9, "sniff TX event id %d len %d", sniff_last_tx_id, sniff_last_tx_len);
    dma_complete(-2);
    CHECK(!cs_low_a && !cs_low_b, "CS released");
    CHECK(LIVE_SENSOR_DATA[0] == 0xFFFFFFFEu, "channel 0 = -2 sign extended, got %08X", LIVE_SENSOR_DATA[0]);
    CHECK(sniff_last_rx_len == 3, "sniff RX len");
    CHECK(LiveSlots_Flags(0) == 0, "fresh slot flags %u", LiveSlots_Flags(0));
    CHECK(regs[0][2] == 0x7C, "WREG applied: AFE A now converts channel 1");

    /* ---- a full lap on A: values land in the generated slot order (0,1,2,3,4,9,10,13) ---------------------------- */
    static const int a_slots[8] = { 0, 1, 2, 3, 4, 9, 10, 13 };
    for (int i = 1; i < 17; i++) {   /* channel 0 done above; continue at channel 1 */
        drdy(0);
        dma_complete(0x100 + i);
        int slot = a_slots[i % 8];
        CHECK(LIVE_SENSOR_DATA[slot] == (uint32_t)(0x100 + i), "A sample %d -> slot %d = %X", i, slot, LIVE_SENSOR_DATA[slot]);
    }

    /* ---- B: CS_B, 0x800000 is the most negative code -------------------------------------------------------------- */
    drdy(1);
    CHECK(dma_active && dma_afe == 1 && cs_low_b && !cs_low_a, "B DRDY must assert CS_B only");
    CHECK(sniff_last_tx_id == 1, "sniff TX id for B is %d", sniff_last_tx_id);
    dma_complete(-8388608);
    CHECK(LIVE_SENSOR_DATA[6] == 0xFF800000u, "B channel 0 (slot 6) = %08X", LIVE_SENSOR_DATA[6]);
    CHECK(st->samples[0] == 17 && st->samples[1] == 1, "sample counters %u %u", st->samples[0], st->samples[1]);

    /* ---- both DRDYs at once: the second waits, then starts from the completion; repeated edge = overrun ------------- */
    drdy(0);
    drdy(1);
    drdy(1);
    CHECK(dma_afe == 0 && st->drdy_overruns == 1, "overrun counted: %u", st->drdy_overruns);
    dma_complete(1);
    CHECK(dma_active && dma_afe == 1 && cs_low_b && !cs_low_a, "pending B starts right after A");
    dma_complete(2);
    CHECK(!dma_active, "idle again");

    /* ---- DMA start failure: CS released, counted, the same channel is retried by the next DRDY -------------------- */
    uint32_t err0 = st->spi_errors;
    dma_result = HAL_BUSY;
    drdy(0);
    CHECK(!cs_low_a && !cs_low_b && st->spi_errors == err0 + 1, "start failure handling");
    dma_result = HAL_OK;
    uint32_t a_before = st->samples[0];
    drdy(0);
    dma_complete(0x77);
    CHECK(st->samples[0] == a_before + 1, "driver continues after a start failure");

    /* ---- SPI error mid transfer: resync from the main loop (STOP, channel 0, START) -------------------------------- */
    drdy(0);
    HAL_SPI_ErrorCallback(&hspi2);
    CHECK(!cs_low_a && !cs_low_b, "CS released by the error callback");
    memset(g_log, 0, sizeof g_log);
    uint32_t rs0 = st->resyncs;
    AfeService();
    CHECK(st->resyncs == rs0 + 1, "resync counted");
    CHECK(strstr(g_log, "A:STOP") && strstr(g_log, "A:WREG") && strstr(g_log, "A:START"), "resync sequence: %s", g_log);
    CHECK(strstr(g_log, "A:STOP") < strstr(g_log, "A:START"), "STOP before START");
    CHECK(regs[0][2] == 0x9C, "channel 0 loaded again");
    drdy(0);
    dma_complete(0x55);
    CHECK(LIVE_SENSOR_DATA[0] == 0x55, "after resync the first sample belongs to channel 0 (slot 0): %X", LIVE_SENSOR_DATA[0]);

    /* ---- no-sample watchdog restarts both chains ------------------------------------------------------------------ */
    memset(g_log, 0, sizeof g_log);
    g_tick += 100;
    rs0 = st->resyncs;
    AfeService();
    CHECK(st->resyncs == rs0 + 2, "watchdog resyncs both AFEs: %u", st->resyncs - rs0);
    CHECK(strstr(g_log, "A:START") && strstr(g_log, "B:START"), "restart log %s", g_log);
    AfeService();
    CHECK(st->resyncs == rs0 + 2, "no restart loop");

    /* ---- staleness window of a slot (A: 3 cycles + margin = 12 ms) ------------------------------------------------- */
    drdy(0); dma_complete(5);                  /* slot 0 fresh */
    g_tick += 8;
    CHECK(LiveSlots_Flags(0) == 0, "still fresh after 8 ms");
    g_tick += 6;
    CHECK(LiveSlots_Flags(0) == LIVE_FLAG_STALE, "stale after 14 ms");

    /* ---- a DMA transfer that never finishes is aborted and the chain resynchronised --------------------------------- */
    drdy(1);
    CHECK(dma_active, "B transfer running");
    abort_calls = 0;
    g_tick += 10;
    uint32_t to0 = st->xfer_timeouts;
    AfeService();
    CHECK(st->xfer_timeouts == to0 + 1 && abort_calls >= 1 && !dma_active && !cs_low_b, "stuck transfer recovered");
    drdy(1);
    dma_complete(9);
    CHECK(LIVE_SENSOR_DATA[6] == 9, "B delivers again after the recovery");

    /* ---- wrong device ID on B: B is not started, A is ------------------------------------------------------------- */
    reset_world();
    dev_id[1] = 0x05;
    CHECK(!AfeInit(), "AfeInit must fail with a wrong ID");
    CHECK((st->status & AFE_ST_B_ID_OK) == 0 && (st->status & AFE_ST_B_RUNNING) == 0 && (st->status & AFE_ST_A_RUNNING), "status 0x%02X", st->status);
    CHECK(strstr(g_log, "A:START") && !strstr(g_log, "B:START"), "START only for A: %s", g_log);
    dma_starts = 0;
    drdy(1);
    CHECK(dma_starts == 0 && !cs_low_b, "DRDY of an AFE that did not come up is ignored");

    /* ---- B fails its channel 0 read-back at boot: the failing step is reported and a later retry brings it up ------- */
    reset_world();
    corrupt_b_reg4 = 1;
    CHECK(!AfeInit(), "AfeInit must report the B failure");
    CHECK(st->init_stage[AFE_B] == AFE_INIT_LOAD_VERIFY && st->init_reg[AFE_B] == 2 && st->init_got[AFE_B] == (uint8_t)(st->init_exp[AFE_B] ^ 0x01),
          "B stage %u reg %u got %02X exp %02X", st->init_stage[AFE_B], st->init_reg[AFE_B], st->init_got[AFE_B], st->init_exp[AFE_B]);
    CHECK(st->init_stage[AFE_A] == AFE_INIT_OK && (st->status & AFE_ST_A_RUNNING) && !(st->status & AFE_ST_B_RUNNING), "status 0x%02X", st->status);
    CHECK(strstr(g_log, "A:START") && !strstr(g_log, "B:START"), "START only for A: %s", g_log);
    AfeService();                                   /* too early: no retry before AFE_INIT_RETRY_MS */
    CHECK(st->init_retries == 0, "retried too early");
    corrupt_b_reg4 = 0;                             /* the fault goes away */
    g_tick += 5100;
    AfeService();
    CHECK(st->init_retries == 1 && (st->status & AFE_ST_B_RUNNING) && st->init_stage[AFE_B] == AFE_INIT_OK, "retry: status 0x%02X stage %u retries %u",
          st->status, st->init_stage[AFE_B], st->init_retries);
    CHECK(strstr(g_log, "B:START"), "B started by the retry: %s", g_log);
    CHECK(!cs_low_a && !cs_low_b, "bus released after the retry");
    dma_starts = 0;
    drdy(1);
    CHECK(dma_starts == 1, "DRDY of B is serviced after the retry");
    dma_complete(9);

    /* ---- B does not answer at all (reads zeros, the ADS124S08 ID is 000 too): reported as no response, not as "ID ok" ---- */
    reset_world();
    b_dead = 1;
    CHECK(!AfeInit(), "AfeInit must fail when B is dead");
    CHECK(st->init_stage[AFE_B] == AFE_INIT_NO_RESPONSE, "B stage %u", st->init_stage[AFE_B]);
    CHECK(!(st->status & (AFE_ST_B_ID_OK | AFE_ST_B_RUNNING)) && (st->status & AFE_ST_A_ID_OK) && (st->status & AFE_ST_A_RUNNING), "status 0x%02X", st->status);

    /* ---- SPI2 not in mode 1: refuse to start ------------------------------------------------------------------------ */
    reset_world();
    hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
    CHECK(!AfeInit() && (st->status & AFE_ST_SPI_MODE_BAD) && g_log[0] == 0, "mode check: status 0x%02X log '%s'", st->status, g_log);
    drdy(0);
    CHECK(!dma_active, "nothing runs");

    printf(g_fail ? "SIM FAILED (%d)\n" : "SIM OK\n", g_fail);
    return g_fail ? 1 : 0;
}
