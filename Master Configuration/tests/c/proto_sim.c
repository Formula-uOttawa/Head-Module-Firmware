/* Host UART simulator: raw bytes in on stdin, raw bytes out on stdout. Real firmware framing + dispatcher, fake platform. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "head_cfg.h"
#include "hframe.h"
#include "hproto.h"

static uint8_t stage[HEAD_MAX_BLOB], active[HEAD_MAX_BLOB], stored[HEAD_MAX_BLOB];
static uint32_t active_len, stored_len;
static uint8_t active_src;
static HeadConfig_t scratch;
static Hproto_t proto;
static HframeRx_t rx;
static int sniff_pending;

static void send_msg(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n)
{
    uint8_t out[HFRAME_MAX_ENCODED];
    size_t w = Hframe_Encode(type, seq, payload, n, out, sizeof out);
    if (w) { ssize_t r = write(1, out, w); (void)r; }
}

static void get_info(void *c, HprotoInfo_t *i)
{
    (void)c;
    i->fw_version = 0x0102;
    i->uptime_ms = 1234;
    i->active_hash = active_len ? HeadCfg_BlobHash(active, active_len) : 0xDEFA0171DEFA0171ull;
    i->stored_hash = stored_len ? HeadCfg_BlobHash(stored, stored_len) : 0;
    i->active_source = active_src;
    i->cm4_alive = 1;
    i->channel_count = 3;
    i->fdcan_kernel_hz = 80000000u;
    for (int k = 0; k < 12; k++) i->uid[k] = (uint8_t)k;
    i->cm4_status = 0x02; i->boot_apply_rc = 2; i->boot_apply_tries = 300;
    i->hframe_crc_fail = 11; i->hframe_overflow = 12; i->hframe_desync = 13;
    i->rx_ring_overflow = 14; i->uart_err = 15; i->uart_tx_fail = 16;
    i->tcan_init_status = 0x24;
}

static int fail_store; /* env HPROTO_SIM_FAIL_STORE: the flash write fails after a successful apply */

static uint8_t cfg_commit(void *c, const uint8_t *blob, uint32_t len, uint8_t flags, uint64_t *hash, uint8_t *applied, uint8_t *st,
                          uint8_t *store_err)
{
    (void)c;
    if (HeadCfg_Parse(blob, len, &scratch) != HEAD_CFG_OK) return HERR_INVALID_CONFIG;
    *hash = scratch.config_hash;
    if (flags & HPROTO_COMMIT_APPLY) { memcpy(active, blob, len); active_len = len; active_src = 2; *applied = 1; }
    if (flags & HPROTO_COMMIT_STORE) {
        if (fail_store) {
            if (!*applied) return HERR_FLASH;
            *store_err = HERR_FLASH;
            return 0;
        }
        memcpy(stored, blob, len); stored_len = len; *st = 1; if (*applied) active_src = 1;
    }
    return 0;
}

static uint8_t cfg_read(void *c, uint8_t source, uint32_t off, uint8_t *dst, uint16_t max, uint32_t *total, uint16_t *n)
{
    (void)c;
    const uint8_t *src = source ? stored : active;
    uint32_t len = source ? stored_len : active_len;
    if (!len || off > len) return HERR_RANGE;
    *total = len;
    *n = (uint16_t)(len - off < max ? len - off : max);
    memcpy(dst, src + off, *n);
    return 0;
}

static uint8_t cfg_clear(void *c, uint8_t flags)
{
    (void)c;
    stored_len = 0;
    if (flags & HPROTO_CLEAR_REVERT) { active_len = 0; active_src = 0; }
    return 0;
}

static uint16_t live_get(void *c, uint16_t first, uint16_t count, uint8_t *dst)
{
    (void)c;
    for (uint16_t i = 0; i < count; i++) {
        uint32_t v = (uint32_t)(first + i) * 1000u + 7u;
        for (int b = 0; b < 4; b++) dst[5 * i + b] = (uint8_t)(v >> (8 * b));
        dst[5 * i + 4] = (uint8_t)((first + i) & 1);
    }
    return count;
}

static uint16_t stats_get(void *c, uint8_t *dst, uint16_t cap)
{
    (void)c;
    if (getenv("HPROTO_SIM_STATS_FULL")) { /* the real encoder with distinctive values, 2 buses */
        static HeadStats_t st;
        memset(&st, 0, sizeof st);
        for (int b = 0; b < 2; b++) {
            st.bus[b].rx_frames = 100u + b; st.bus[b].rx_matched = 200u + b; st.bus[b].rx_dropped = 300u + b; st.bus[b].fifo_lost = 400u + b;
            st.bus[b].tx_frames = 500u + b; st.bus[b].tx_dropped = 600u + b; st.bus[b].bus_off = (uint8_t)b; st.bus[b].passive = (uint8_t)(1 - b);
        }
        for (int h = 0; h < 8; h++) st.hub_lost[h] = 700u + h;
        st.rx_nobus = 21; st.rx_ring_overrun = 22; st.rx_ring_high = 23;
        return Hproto_EncodeStats(&st, 2, dst, cap);
    }
    memset(dst, 0, 1 + 18 + 1 + 32);
    dst[0] = 1;
    dst[1] = 5; dst[5] = 4; dst[9] = 1; dst[13] = 2; dst[17] = 0; dst[18] = 1; /* rx=5 matched=4 dropped=1 lost=2 bus_off=0 passive=1 */
    dst[19] = 8;
    dst[20] = 9; /* hub_lost[0] = 9 */
    return 1 + 18 + 1 + 32;
}

static uint8_t sniff_set(void *c, uint8_t bus, uint8_t en)
{
    (void)c;
    if (bus > 1) return HERR_RANGE;
    sniff_pending = en;
    return 0;
}

static void sim_reboot(void *c) { (void)c; }

static const HprotoOps_t ops = { get_info, cfg_commit, cfg_read, cfg_clear, live_get, stats_get, sniff_set, sim_reboot };

static void on_msg(void *c, const uint8_t *msg, size_t len)
{
    (void)c;
    uint8_t resp[HFRAME_MAX_BODY];
    size_t n = Hproto_Handle(&proto, msg, len, resp, sizeof resp);
    if (n) send_msg(resp[0], resp[1], resp + 2, n - 2);
    if (sniff_pending) { /* unsolicited sniff frame: bus 1, ext, id 0x18FF1234, ts 77, 3 bytes */
        const uint8_t p[] = { 1, 1, 0x34, 0x12, 0xFF, 0x18, 77, 0, 0, 0, 3, 0xAA, 0xBB, 0xCC };
        send_msg(HMSG_SNIFF_FRAME, 0, p, sizeof p);
        sniff_pending = 0;
    }
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
    fail_store = getenv("HPROTO_SIM_FAIL_STORE") != NULL;
    Hproto_Init(&proto, &ops, NULL, stage, sizeof stage);
    HframeRx_Init(&rx, on_msg, on_line, NULL);
    uint8_t buf[256];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0)
        for (ssize_t i = 0; i < n; i++) HframeRx_Feed(&rx, buf[i]);
    return 0;
}
