#include <stdint.h>
#include <stdbool.h>

// Not exhaustive, doesnt contain definitions for all registers.
typedef enum {
    DEVICE_ID = 0x0,
    REV_ID = 0x8,
    SPI_CONFIG = 0x9,
    CRC_CNTL = 0xA,
    CRC_POLY_SET = 0xB,
    SBC_CONFIG = 0xC,
    SCRATCH_PAD_SPI = 0xF,
    CAN_CNTRL_1 = 0x10,
    WD_CONFIG_1 = 0x13,
    WD_CONFIG_2 = 0x14,
    WD_INPUT_TRIG = 0x15,
    INT_GLOBAL = 0x50,
    INT_1 = 0x51,
    INT_2 = 0x52,
    INT_3 = 0x53,
    INT_CANBUS_1 = 0x54,
    INT_7 = 0x55,
    INT_EN_1 = 0x56,
    INT_EN_2 = 0x57,
    INT_EN_3 = 0x58,
    INT_EN_CANBUS_1 = 0x59,
    INT_4 = 0x5A,
    INT_6 = 0x5C,
    INT_EN_4 = 0x5E,
    INT_EN_6 = 0x60,
    INT_EN_7 = 0x62,
} TCanReg_t;

#define SBC_STARTUP_CFG 0b10001010 // normal mode, vcc2 on
#define WD_DISABLED 0b00000010 // disabled, 600ms window

bool TCAN2410Init(void);
bool TCANSpiWrite(uint8_t reg, uint8_t val);
bool TCANSpiRead(uint8_t reg, uint8_t *val);