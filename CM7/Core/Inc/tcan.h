/*
 * tcan.h - driver for the TCAN2410-Q1 system basis chip (SBC) on SPI1 (CM7): watchdog switch-off, normal mode, CAN transceiver.
 * Hardened port of the hub module driver. The head module's FDCAN transceivers may not be the SBC's; the CAN_CNTRL_1 step is
 * harmless if they are not.
 */
#ifndef TCAN_H
#define TCAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "head_config.h"

// Not exhaustive, doesnt contain definitions for all registers.
typedef enum {
    TCAN_REG_DEVICE_ID = 0x0,
    TCAN_REG_REV_ID = 0x8,
    TCAN_REG_SPI_CONFIG = 0x9,
    TCAN_REG_CRC_CNTL = 0xA,
    TCAN_REG_CRC_POLY_SET = 0xB,
    TCAN_REG_SBC_CONFIG = 0xC,
    TCAN_REG_SCRATCH_PAD_SPI = 0xF,
    TCAN_REG_EEPROM_CTRL = 0x4E,
    TCAN_REG_HSS_CNTL3 = 0x4F, // bit 1 = VCAN_STATUS
    TCAN_REG_INT_4_REG = 0x5A, // bit 1 = EEPROM_CRC_INT
    TCAN_REG_CAN_CNTRL_1 = 0x10,
    TCAN_REG_WD_CONFIG_1 = 0x13,
    TCAN_REG_WD_CONFIG_2 = 0x14,
    TCAN_REG_WD_INPUT_TRIG = 0x15,
    TCAN_REG_INT_GLOBAL = 0x50,
    TCAN_REG_INT_1 = 0x51,
    TCAN_REG_INT_2 = 0x52,
    TCAN_REG_INT_3 = 0x53,
    TCAN_REG_INT_CANBUS_1 = 0x54,
    TCAN_REG_INT_7 = 0x55,
    TCAN_REG_INT_EN_1 = 0x56,
    TCAN_REG_INT_EN_2 = 0x57,
    TCAN_REG_INT_EN_3 = 0x58,
    TCAN_REG_INT_EN_CANBUS_1 = 0x59,
    TCAN_REG_INT_4 = 0x5A,
    TCAN_REG_INT_6 = 0x5C,
    TCAN_REG_INT_EN_4 = 0x5E,
    TCAN_REG_INT_EN_6 = 0x60,
    TCAN_REG_INT_EN_7 = 0x62,
} TCanReg_t;

#define SBC_STARTUP_CFG 0b10001010 // normal mode, vcc2 on
#define WD_DISABLED 0b00000010 // WD_CONFIG (bits 7-6) = 00 disabled; standby long window stays 600ms
#define WD_STBY_DIS 0x01 // WD_CONFIG_2 bit 0: disable the watchdog in standby mode
#define VCAN_STATUS_BIT 0x02 // HSS_CNTL3 bit 1: 5 V VCAN supply of the CAN transmitter present
// CAN_CNTRL_1 (datasheet 9.1.10): CAN1_TRX_SEL = 110b "SBC mode control" = transmitter ON in SBC normal mode (wake capable
// in standby/sleep/restart/fail-safe, Table 8-1). The reset value 100b is "wake capable": the driver stays Z/Z (Table 8-2).
// CAN1_FSM_DIS (bit 3) = 0: wake capable (passive) in fail-safe mode; TXD dominant timeout stays enabled.
#define CAN_CNTRL_1_TRX_MASK 0x07
#define CAN_CNTRL_1_ON_NORMAL 0x06

// bits of tcan_init_status, set by TCAN2410Init()
#define TCAN_INIT_WD_RETRIED 0x01     // watchdog disable needed more than one attempt
#define TCAN_INIT_WD_FAILED 0x02      // watchdog could not be disabled (SPI not working?)
#define TCAN_INIT_FAILED 0x04         // TCAN2410Init() returned false (set on every failure path; head module addition)
#define TCAN_INIT_MODE_FAILED 0x08    // could not write SBC_CONFIG
#define TCAN_INIT_STBY_NOT_DISABLED 0x10 // WD_STBY_DIS did not stick (register protected?); start-up continues
#define TCAN_INIT_WARM_START 0x20     // SBC already had the watchdog off (MCU reset without SBC power cycle); registers left alone
#define TCAN_INIT_NO_VCAN 0x40        // VCAN_STATUS = 0: the CAN transmitter has no 5 V supply (init still succeeds)
#define TCAN_INIT_CAN_FAILED 0x80     // CAN_CNTRL_1 could not be written / did not read back as ON in normal mode
extern uint8_t tcan_init_status;
extern uint8_t tcan_global_int; // SPI status byte (INT_GLOBAL style) returned with the last transfer
extern uint8_t tcan_int_snapshot[4];

typedef enum {
    TCAN_SAVE_OK = 0,
    TCAN_SAVE_NO_LINK,        // scratch pad round trip failed
    TCAN_SAVE_WD_NOT_OFF,     // registers 0x13/0x14 do not currently hold "watchdog off" (nothing sensible to save)
    TCAN_SAVE_NO_VCAN,        // VCAN supply not in regulation, EEPROM cannot be written
    TCAN_SAVE_ALREADY_SAVED,  // the SBC booted with the watchdog already off from EEPROM; pass force to save anyway
    TCAN_SAVE_CRC_FAILED,     // could not enable SPI CRC, nothing was written
    TCAN_SAVE_NOT_ACCEPTED,   // SBC reported a SPI/CRC error for the save frame
    TCAN_SAVE_TIMEOUT,        // EEPROM write did not finish
    TCAN_SAVE_CRC_STUCK,      // SPI CRC could not be switched off again: reset the SBC (nRST) before using SPI
    TCAN_SAVE_VERIFY_FAILED   // EEPROM CRC check flagged an error after the save
} TCanSaveStatus_t;

/*
 * HEAD_TEMP_SAVE_WD_EEPROM (head_config.h) ONE-SHOT: 1 = at boot, after the watchdog has been switched off (SBC still in
 * standby), store that setting in the SBC's EEPROM (see TCANSaveWatchdogConfig). The result is printed as
 * "boot:tcan:save=<code>" on the debug UART. Needed once per board. Afterwards set it back to 0 and rebuild; leaving it
 * at 1 is harmless (a later boot just reports ERR:4, "already saved"). There is no runtime command for it.
 */
extern int tcan_temp_save_result; // -1 = not attempted, else a TCanSaveStatus_t

// Brings the SBC up: watchdog off, normal mode, CAN transceiver ON (CAN_CNTRL_1). false = the CAN path is not usable.
// Reports in tcan_init_status. The first thing to call after reset (the SBC's standby watchdog is running).
bool TCAN2410Init(void);
// Stores the "watchdog off" configuration (registers 0x13/0x14, as left by TCAN2410Init) in the SBC's EEPROM, so every
// power-up and nRST reset reloads it. The EEPROM can only be written 500 times; do this once per board.
TCanSaveStatus_t TCANSaveWatchdogConfig(bool force);
extern bool tcan_init_ok;          // result of the last TCAN2410Init()
extern bool tcan_eeprom_wd_loaded; // SBC came up with the watchdog already off (EEPROM config present)
bool TCANSpiWrite(uint8_t reg, uint8_t val);
bool TCANSpiRead(uint8_t reg, uint8_t *val);


// Registers the ASCII console ("send:tcan:RRVV") may write: the scratch pad, CAN_CNTRL_1 and the interrupt FLAG registers
// (write 1 to clear). Everything else (SBC_CONFIG, watchdog, interrupt enables, EEPROM, power rails) is refused: one typo there
// can cut the module's power. RR is the register NUMBER as in TCanReg_t; the driver forms the SPI address byte (reg << 1 | W).
static inline bool TCANConsoleWriteAllowed(uint8_t reg) {
    switch (reg) {
    case TCAN_REG_SCRATCH_PAD_SPI:
    case TCAN_REG_CAN_CNTRL_1:
    case TCAN_REG_INT_GLOBAL:
    case TCAN_REG_INT_1:
    case TCAN_REG_INT_2:
    case TCAN_REG_INT_3:
    case TCAN_REG_INT_CANBUS_1:
    case TCAN_REG_INT_7:
    case TCAN_REG_INT_4:
    case TCAN_REG_INT_6:
        return true;
    default:
        return false;
    }
}

#endif /* TCAN_H */
