#include "head_debug.h"

#include <stdio.h>
#include <string.h>

#include "head_link.h"
#include "head_store.h"
#include "head_text.h"
#include "main.h"
#include "tcan.h"

/*
 * Commands (case sensitive, "name:arg:arg", terminated by CR/LF):
 *   ping
 *   send:tcan:RRVV   write VV to SBC register RR (both hex, exactly 4 hex digits). RR is the register NUMBER as in TCanReg_t
 *                    (the driver builds the SPI address byte reg << 1 | 1); only registers in TCANConsoleWriteAllowed() are accepted.
 *   tcan:wd          read-only: watchdog registers, init status and the SPI status byte
 *   flash            read-only: last flash status of the config store
 *   reset
 */

static void reply_err(char *resp, size_t cap, const char *cmd, const char *what, uint32_t ts)
{
    snprintf(resp, cap, "%s:%s:%lu\r\n", cmd, what, (unsigned long)ts);
}

void HeadDebug_HandleLine(const char *line)
{
    char buf[128], resp[160];
    uint32_t ts = HAL_GetTick();

    resp[0] = '\0';
    if (!HeadText_IsPrintable(line)) /* binary garbage must never be executed or echoed */
        return;
    strncpy(buf, line, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';

    char *save = NULL;
    char *cmd = strtok_r(buf, ":", &save);
    if (cmd == NULL)
        return;

    if (strcmp(cmd, "ping") == 0) {
        snprintf(resp, sizeof resp, "ping:OK:%lu\r\n", (unsigned long)ts);
    } else if (strcmp(cmd, "send") == 0) {
        char *target = strtok_r(NULL, ":", &save);
        if (target && strcmp(target, "tcan") == 0) {
            char *hex = strtok_r(NULL, ":", &save);
            uint8_t reg = 0, val = 0;
            if (!HeadText_ParseHex4(hex, &reg, &val)) {
                reply_err(resp, sizeof resp, "send", "ERR_BAD_ARGS", ts);
            } else if (!TCANConsoleWriteAllowed(reg)) {
                reply_err(resp, sizeof resp, "send", "ERR_REG_NOT_ALLOWED", ts);
            } else if (!TCANSpiWrite(reg, val)) {
                reply_err(resp, sizeof resp, "send", "ERR_SPI", ts);
            } else {
                snprintf(resp, sizeof resp, "send:OK:%lu:%u\r\n", (unsigned long)ts, (unsigned)tcan_global_int);
            }
        } else { /* fdcan / afea / afeb: not implemented */
            reply_err(resp, sizeof resp, "send", "ERR_UNSUPPORTED", ts);
        }
    } else if (strcmp(cmd, "tcan") == 0) {
        char *sub = strtok_r(NULL, ":", &save);
        if (sub && strcmp(sub, "wd") == 0) {
            uint8_t w1 = 0, w2 = 0;
            bool ok = TCANSpiRead(TCAN_REG_WD_CONFIG_1, &w1) && TCANSpiRead(TCAN_REG_WD_CONFIG_2, &w2);
            if (ok)
                snprintf(resp, sizeof resp, "tcan:wd:OK:%lu:wd1=%02X:wd2=%02X:init=%02X:st=%02X\r\n", (unsigned long)ts, (unsigned)w1,
                         (unsigned)w2, (unsigned)tcan_init_status, (unsigned)tcan_global_int);
            else
                reply_err(resp, sizeof resp, "tcan:wd", "ERR_SPI", ts);
        } else {
            reply_err(resp, sizeof resp, "tcan", "ERR_BAD_ARGS", ts);
        }
    } else if (strcmp(cmd, "flash") == 0) {
        snprintf(resp, sizeof resp, "flash:OK:%lu:hal=%u:err=%08lX\r\n", (unsigned long)ts, (unsigned)HeadStore_LastHalStatus(),
                 (unsigned long)HeadStore_LastFlashError());
    } else if (strcmp(cmd, "reset") == 0) {
        snprintf(resp, sizeof resp, "reset:OK:%lu\r\n", (unsigned long)ts);
        HeadLink_SendText(resp); /* reply first: the reset stops the MCU */
        HAL_NVIC_SystemReset();
        return;
    } else {
        reply_err(resp, sizeof resp, cmd, "ERR_BAD_CMD", ts); /* cmd is printable ASCII and at most 127 bytes */
    }
    HeadLink_SendText(resp);
}

void HeadDebug_BootReport(bool tcan_ok, unsigned tries)
{
    char resp[96];
    snprintf(resp, sizeof resp, "boot:tcan:%s:tries=%u:init=%02X:int=%02X,%02X,%02X,%02X\r\n", tcan_ok ? "OK" : "FAIL", tries,
             (unsigned)tcan_init_status, (unsigned)tcan_int_snapshot[0], (unsigned)tcan_int_snapshot[1], (unsigned)tcan_int_snapshot[2],
             (unsigned)tcan_int_snapshot[3]);
    HeadLink_SendText(resp);
#if HEAD_TEMP_SAVE_WD_EEPROM
    snprintf(resp, sizeof resp, "boot:tcan:save=%d\r\n", tcan_temp_save_result);
    HeadLink_SendText(resp);
#endif
}
