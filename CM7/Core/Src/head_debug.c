#include "head_debug.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "head_link.h"
#include "main.h"
#include "tcan.h"

extern uint8_t tcan_global_int;

volatile bool sniff_tcan, sniff_afeA, sniff_afeB;

static uint8_t hex_val(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; hex[i] && hex[i + 1] && n < max; i += 2)
        out[n++] = (uint8_t)((hex_val(hex[i]) << 4) | hex_val(hex[i + 1]));
    return n;
}

void HeadDebug_HandleLine(const char *line)
{
    char buf[128], resp[160];
    uint32_t ts = HAL_GetTick();
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
            uint8_t b[2] = { 0, 0 };
            if (hex == NULL || hex_to_bytes(hex, b, 2) != 2) {
                snprintf(resp, sizeof resp, "send:ERR_BAD_ARGS:%lu\r\n", (unsigned long)ts);
            } else {
                TCANSpiWrite(b[0], b[1]);
                snprintf(resp, sizeof resp, "send:OK:%lu:%u\r\n", (unsigned long)ts, (unsigned)tcan_global_int);
            }
        } else { /* fdcan / afea / afeb: not implemented in the prototype either */
            snprintf(resp, sizeof resp, "send:ERR_UNSUPPORTED:%lu\r\n", (unsigned long)ts);
        }
    } else if (strcmp(cmd, "sniff") == 0) {
        char *target = strtok_r(NULL, ":", &save);
        char *action = strtok_r(NULL, ":", &save);
        if (target == NULL || action == NULL) {
            snprintf(resp, sizeof resp, "sniff:ERR_BAD_ARGS:%lu\r\n", (unsigned long)ts);
        } else {
            bool on = (strcmp(action, "on") == 0 || strcmp(action, "ON") == 0);
            const char *name = NULL;
            if (strcmp(target, "tcan") == 0) { sniff_tcan = on; name = "tcan"; }
            else if (strcmp(target, "afea") == 0) { sniff_afeA = on; name = "afeA"; }
            else if (strcmp(target, "afeb") == 0) { sniff_afeB = on; name = "afeB"; }
            if (name)
                snprintf(resp, sizeof resp, "sniff:%s:%s:%lu\r\n", name, on ? "ON" : "OFF", (unsigned long)ts);
            else
                snprintf(resp, sizeof resp, "sniff:ERR_BAD_TARGET:%lu\r\n", (unsigned long)ts);
        }
    } else if (strcmp(cmd, "reset") == 0) {
        snprintf(resp, sizeof resp, "reset:OK:%lu\r\n", (unsigned long)ts);
        HeadLink_SendText(resp); /* reply first: the reset stops the MCU */
        HAL_NVIC_SystemReset();
        return;
    } else {
        snprintf(resp, sizeof resp, "%s:ERR_BAD_CMD:%lu\r\n", cmd, (unsigned long)ts);
    }
    HeadLink_SendText(resp);
}
