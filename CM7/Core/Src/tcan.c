#include "tcan.h"
#include "main.h"
// #include "debug_sniff.h"

extern SPI_HandleTypeDef hspi1;

#define TCAN_BUFFER_SIZE 2
uint8_t tcan_tx_buffer[TCAN_BUFFER_SIZE];
uint8_t tcan_rx_buffer[TCAN_BUFFER_SIZE];
uint8_t tcan_global_int;

// TODO: implement interrupt from tcan device

// Initialize the TCAN interface, turn off the SPI
bool TCAN2410Init(void) {

    // disable watchdog for debugging
    TCANSpiWrite(WD_CONFIG_1, WD_DISABLED); 

    uint8_t device_id;
    TCANSpiRead(DEVICE_ID, &device_id);

    // Clear power-on and fail-safe interrupt flags
    TCANSpiWrite(INT_1, 0xFF);
    TCANSpiWrite(INT_2, 0xFF);
    TCANSpiWrite(INT_GLOBAL, 0xFF);

    // quick spi test using the spi scratch pad, perform a write and readback
    uint8_t test;
    if (!TCANSpiWrite(SCRATCH_PAD_SPI, 0xA5)) return false;
    if (!TCANSpiRead(SCRATCH_PAD_SPI, &test)) return false;
    if (test != 0xA5) return false;
    test = 0;
    if (!TCANSpiWrite(SCRATCH_PAD_SPI, 0x5A)) return false;
    if (!TCANSpiRead(SCRATCH_PAD_SPI, &test)) return false;
    if (test != 0x5A) return false;
    
    // clear power on interrupt
    TCANSpiWrite(INT_2, 0xFF);
    if (tcan_global_int != 0) {
        // perhaps check which interrupt is on?
    }
    
    // set sbc into normal mode
    TCANSpiWrite(SBC_CONFIG, SBC_STARTUP_CFG);
    
    return true;
}

bool TCANSpiWrite(uint8_t reg, uint8_t val) {
    tcan_tx_buffer[0] = reg << 1 | 1;
    tcan_tx_buffer[1] = val;

    tcan_rx_buffer[0] = 0;
    tcan_rx_buffer[1] = 0;

    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);

    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi1, tcan_tx_buffer, tcan_rx_buffer, TCAN_BUFFER_SIZE, HAL_MAX_DELAY);
    
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);

    if (status != HAL_OK) return false;

    tcan_global_int = tcan_rx_buffer[0];
    
    // Emit sniff event for TCAN write (reg/value)
    // {
    //     uint8_t dbg_data[2];
    //     // tx buffer contains reg<<1|1 in [0], value in [1]
    //     dbg_data[0] = (tcan_tx_buffer[0] >> 1) & 0xFF;
    //     dbg_data[1] = tcan_tx_buffer[1];
    //     debug_sniff_push(SNIFF_TCAN_TX, 0, dbg_data, 2);
    // }
    
    // TODO: implement CRC
    
    return true;
}

bool TCANSpiRead(uint8_t reg, uint8_t *val) {
    tcan_tx_buffer[0] = reg << 1 | 0;
    tcan_tx_buffer[1] = 0;

    tcan_rx_buffer[0] = 0;
    tcan_rx_buffer[1] = 0;

    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);

    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi1, tcan_tx_buffer, tcan_rx_buffer, TCAN_BUFFER_SIZE, HAL_MAX_DELAY);
    
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);

    if (status != HAL_OK) return false;

    tcan_global_int = tcan_rx_buffer[0];
    *val = tcan_rx_buffer[1];

    // {
    //     uint8_t dbg_data[2];
    //     dbg_data[0] = (tcan_tx_buffer[0] >> 1) & 0xFF; // requested reg
    //     dbg_data[1] = tcan_rx_buffer[1]; // returned value
    //     debug_sniff_push(SNIFF_TCAN_RX, 0, dbg_data, 2);
    // }

    // TODO: implement CRC

    return true;
}
