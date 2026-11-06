/* Host UART simulator of a hub module: raw bytes in on stdin, raw bytes out on stdout. Real hub_proto.c + hframe.c. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "hframe.h"
#include "hub_proto.h"

static HframeRx_t rx;

static void send_msg(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n)
{
    uint8_t out[HFRAME_MAX_ENCODED];
    size_t w = Hframe_Encode(type, seq, payload, n, out, sizeof out);
    if (w) { ssize_t r = write(1, out, w); (void)r; }
}

static void get_info(void *c, HubInfo_t *i)
{
    (void)c;
    i->fw_version = 0x0103;
    i->hub_id = 2;
    for (int k = 0; k < 12; k++) i->uid[k] = (uint8_t)(0xA0 + k);
    i->uptime_ms = 4321;
    i->flags = HUB_INFO_FLAG_CAN_RUNNING;
    i->a_count = 5; i->b_count = 4; i->dig_count = 1;
    i->tcan_int = 0x10;
    i->tec = 3; i->rec = 1; i->bus_off = 0; i->passive = 1;
    i->sysclk_hz = 160000000u; i->fdcan_kernel_hz = 80000000u;
    i->tx_ok = 100; i->tx_fail = 2; i->tx_skipped = 7; i->tx_invalid = 0;
    /* appended INFO fields (bytes 56..87) */
    i->tx_drop_full = 11; i->afe_spi_err = 12; i->afe_resyncs = 13; i->afe_drdy_overrun = 14;
    i->rx_overflow = 15; i->uart_err = 16; i->uart_tx_fail = 17;
    i->tcan_init_status = 0x60; i->afe_status = 0x3F; i->inputs = 0x15;
    /* appended configuration fields (bytes 88..127) */
    i->active_hash = 0x1122334455667788ull; i->stored_hash = 0; i->active_source = 1; i->cfg_status = 0x83;
    i->canrx_frames = 21; i->canrx_msgs = 22; i->canrx_bad = 23; i->canrx_overrun = 24; i->cantx_fail = 25;
    i->afe_samples[0] = 31; i->afe_samples[1] = 32; i->afe_drdy_edges[0] = 33; i->afe_drdy_edges[1] = 34; i->afe_resyncs_by[0] = 35; i->afe_resyncs_by[1] = 36;
}

static uint16_t live_get(void *c, uint16_t first, uint16_t count, uint8_t *dst)
{
    (void)c;
    uint16_t w = 0;
    for (; w < count && first + w < HUB_LIVE_SLOTS; w++) {
        uint32_t v = 1000u + first + w;
        for (int k = 0; k < 4; k++) dst[5 * w + k] = (uint8_t)(v >> (8 * k));
        dst[5 * w + 4] = (uint8_t)((first + w) == 5 ? 1 : 0); /* slot 5 reports stale */
    }
    return w;
}

static void do_reboot(void *c)
{
    (void)c;
    static const char msg[] = "ascii:reboot-called\n";
    ssize_t r = write(1, msg, sizeof msg - 1);
    (void)r;
}

static const HubProtoOps_t ops = { get_info, live_get, do_reboot, NULL, NULL, NULL, NULL };

static void on_msg(void *c, const uint8_t *msg, size_t len)
{
    (void)c;
    uint8_t resp[HFRAME_MAX_BODY];
    size_t n = HubProto_Handle(&ops, NULL, msg, len, resp, sizeof resp);
    if (n) send_msg(resp[0], resp[1], resp + 2, n - 2);
}

static void on_line(void *c, const char *line)
{
    (void)c;
    char out[HFRAME_MAX_LINE + 16];
    int n = snprintf(out, sizeof out, "ascii:%s\n", line);
    ssize_t r = write(1, out, (size_t)n);
    (void)r;
}

int main(void)
{
    HframeRx_Init(&rx, on_msg, on_line, NULL);
    uint8_t buf[256];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; i++) HframeRx_Feed(&rx, buf[i]);
    return 0;
}
