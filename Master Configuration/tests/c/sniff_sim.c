/* Host test of the hub's debug_sniff.c (real file, DEF_DEBUG on): raw events queued, text built in flush. */
#include <stdio.h>
#include <string.h>
#include "debug_sniff.h"
#include "hub_link.h"
#include "main.h"

volatile bool sniff_tcan, sniff_afeA, sniff_afeB;
static uint32_t g_tick = 1234;
static char g_out[8192];
static int g_calls;
static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

uint32_t HAL_GetTick(void) { return g_tick; }
bool HubLink_TxRaw(const uint8_t *d, size_t n) { g_calls++; strncat(g_out, (const char *)d, n); return true; }

int main(void)
{
    uint8_t d9[9] = { 0x42, 0x06, 0x7C, 0, 0x1D, 0, 0x0F, 0xFF, 0 };
    uint8_t big[40];
    memset(big, 0xAB, sizeof big);

    /* nothing is queued while a bus is not being sniffed */
    debug_sniff_push(SNIFF_AFE_TX, 0, d9, 9);
    debug_sniff_push(SNIFF_TCAN_TX, 0, d9, 2);
    debug_sniff_flush();
    CHECK(g_calls == 0, "events of unsniffed buses must be dropped at push time");

    sniff_afeA = true; sniff_afeB = false; sniff_tcan = true;
    debug_sniff_push(SNIFF_AFE_TX, 0, d9, 9);
    debug_sniff_push(SNIFF_AFE_RX, 1, d9, 3);          /* B is off */
    debug_sniff_push(SNIFF_AFE_RX, 0, big, 40);        /* longer than the buffer: truncated, no overflow */
    debug_sniff_push(SNIFF_TCAN_RX, 0, d9, 2);
    debug_sniff_flush();
    CHECK(g_calls == 2, "flush is bounded: %d messages in one call", g_calls);
    CHECK(strstr(g_out, "SNIFF:AFE:TX:1234:AFE:0:DATA:42067C001D000FFF00\r\n") == g_out, "tx line: %s", g_out);
    debug_sniff_flush();
    CHECK(g_calls == 3, "second flush sends the rest (B event was never queued)");
    CHECK(strstr(g_out, "SNIFF:AFE:RX:1234:AFE:0:DATA:ABABABABABABABABABAB\r\n"), "truncated line: %s", g_out);
    CHECK(strstr(g_out, "SNIFF:TCAN:RX:1234:REG:0x42:VAL:0x06\r\n"), "tcan line: %s", g_out);

    /* queue overflow drops the oldest and counts it */
    g_out[0] = 0; g_calls = 0;
    for (int i = 0; i < 40; i++) { uint8_t b = (uint8_t)i; debug_sniff_push(SNIFF_AFE_RX, 0, &b, 1); }
    CHECK(debug_sniff_dropped() == 24, "dropped %u", debug_sniff_dropped());
    for (int i = 0; i < 10; i++) debug_sniff_flush();
    CHECK(g_calls == 16, "16 queued events remain: %d", g_calls);
    CHECK(strstr(g_out, "DATA:18\r\n") != NULL && strstr(g_out, "DATA:00\r\n") == NULL, "oldest events dropped");

    printf(g_fail ? "SIM FAILED (%d)\n" : "SIM OK\n", g_fail);
    return g_fail ? 1 : 0;
}
