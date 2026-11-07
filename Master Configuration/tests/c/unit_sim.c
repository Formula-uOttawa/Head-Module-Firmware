/* Host unit tests for the portable head firmware (Common/): encode, decode/stale, receive ring, hproto, DLC helpers.
   Hand-built configs bypass HeadCfg_Parse on purpose, to reach states the parser would refuse. Prints "FAIL <name>" lines
   and "unit: N checks, M failed"; exit status 0 only if everything passed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "head_cfg.h"
#include "head_decode.h"
#include "head_encode.h"
#include "head_rxq.h"
#include "head_util.h"
#include "hframe.h"
#include "hproto.h"

static int checks, failed;
#define CHECK(c, name) do { checks++; if (!(c)) { failed++; printf("FAIL %s (line %d)\n", name, __LINE__); } } while (0)

static HeadConfig_t cfg;
static HeadLive_t live;
static HeadStats_t stats;

static void fresh(void)
{
    memset(&cfg, 0, sizeof cfg);
    memset((void *)&live, 0, sizeof live);
    cfg.hub_bus = -1;
    HeadDecode_Reset(&live, &stats);
}

/* ---- CRC-32 ---------------------------------------------------------------------------------------------------------- */
static uint32_t crc_bitwise(uint32_t crc, const uint8_t *d, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void test_crc(void)
{
    static uint8_t buf[5000];
    srand(77);
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)rand();
    CHECK(HeadCfg_Crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    CHECK(HeadCfg_Crc32(0, buf, 0) == 0, "CRC of nothing");
    for (unsigned n = 0; n < 300; n++) CHECK(HeadCfg_Crc32(0, buf, n) == crc_bitwise(0, buf, n), "table CRC == bitwise CRC");
    CHECK(HeadCfg_Crc32(0, buf, sizeof buf) == crc_bitwise(0, buf, sizeof buf), "table CRC 5000 bytes");
    uint32_t part = HeadCfg_Crc32(0, buf, 1234);
    CHECK(HeadCfg_Crc32(part, buf + 1234, 100) == crc_bitwise(0, buf, 1334), "continued CRC");
}

/* ---- DLC helpers ---------------------------------------------------------------------------------------------------- */
static void test_dlc(void)
{
    static const uint8_t ok[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    unsigned n = 0;
    for (uint32_t l = 0; l < 100; l++) {
        bool expect = false;
        for (unsigned i = 0; i < sizeof ok; i++) expect |= ok[i] == l;
        CHECK(HeadDlc_LenIsValid(l) == expect, "dlc valid set");
        n += expect;
    }
    CHECK(n == 16, "dlc 16 valid lengths");
    CHECK(HeadDlc_LenToCode(9) == 9 && HeadDlc_CodeToLen(9) == 12, "dlc 9 -> 12");
    CHECK(HeadDlc_LenToCode(13) == 10 && HeadDlc_CodeToLen(10) == 16, "dlc 13 -> 16");
    CHECK(HeadDlc_LenToCode(17) == 11 && HeadDlc_CodeToLen(11) == 20, "dlc 17 -> 20");
    CHECK(HeadDlc_LenToCode(64) == 15 && HeadDlc_LenToCode(200) == 15, "dlc 64");
    CHECK(HeadDlc_CodeToLen(0x1F) == 64, "code masked");
}

/* ---- encode --------------------------------------------------------------------------------------------------------- */

/* the previous double-precision implementation, as the reference for bit identity */
static uint32_t old_encode(float scaled, int bits, int is_signed)
{
    double x = scaled >= 0.0f ? (double)scaled + 0.5 : (double)scaled - 0.5;
    double lo, hi;
    if (is_signed) { hi = (double)((1ull << (bits - 1)) - 1ull); lo = -(double)(1ull << (bits - 1)); }
    else { lo = 0.0; hi = (double)((1ull << bits) - 1ull); }
    if (!(x == x)) x = 0.0;
    x = x < lo ? lo : x > hi ? hi : x;
    return (uint32_t)(int64_t)x;
}

static uint32_t build_one(uint8_t vt, uint32_t live_bits, int is_signed, int bits, int is_float, float mult, float off)
{
    fresh();
    cfg.channel_count = 1;
    cfg.channels[0].value_type = vt;
    cfg.bus_count = 1;
    cfg.buses[0].fd = 1;
    cfg.tx_frame_count = 1;
    cfg.tx_frames[0] = (HeadTxFrame_t){ 0, 0, 8, 0x100, 10, 0, 1 };
    cfg.tx_signal_count = 1;
    cfg.tx_signals[0] = (HeadTxSignal_t){ 0, 0, (uint8_t)bits, 0, (uint8_t)is_signed, (uint8_t)is_float, mult, off };
    live.values[0].u = live_bits;
    uint8_t d[64];
    memset(d, 0xAA, sizeof d);
    HeadTx_Build(&cfg, &live, 0, d);
    CHECK(d[8] == 0xAA, "build writes only dlc bytes");
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | d[i];
    return (uint32_t)v;
}

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void test_encode(void)
{
    /* u32 / i32 with multiplier 1, offset 0 take the exact integer path */
    CHECK(build_one(HEAD_VALUE_U32, 16777217u, 0, 32, 0, 1.0f, 0.0f) == 16777217u, "u32 above 2^24 is exact");
    CHECK(build_one(HEAD_VALUE_U32, 0xFFFFFFFFu, 0, 32, 0, 1.0f, 0.0f) == 0xFFFFFFFFu, "u32 max");
    CHECK(build_one(HEAD_VALUE_I32, (uint32_t)-16777217, 1, 32, 0, 1.0f, 0.0f) == (uint32_t)-16777217, "i32 below -2^24 is exact");
    CHECK(build_one(HEAD_VALUE_I32, 0x80000000u, 1, 32, 0, 1.0f, 0.0f) == 0x80000000u, "i32 min");
    CHECK(build_one(HEAD_VALUE_U32, 0xFFFFFFFFu, 0, 8, 0, 1.0f, 0.0f) == 0xFFu, "u32 saturates into 8 bit");
    CHECK(build_one(HEAD_VALUE_I32, (uint32_t)-5, 0, 8, 0, 1.0f, 0.0f) == 0u, "negative into unsigned field -> 0");
    CHECK(build_one(HEAD_VALUE_I32, 200u, 1, 8, 0, 1.0f, 0.0f) == 127u, "i32 200 -> int8 max");
    CHECK(build_one(HEAD_VALUE_I32, (uint32_t)-200, 1, 8, 0, 1.0f, 0.0f) == 0x80u, "i32 -200 -> int8 min");
    CHECK(build_one(HEAD_VALUE_I32, (uint32_t)-1, 1, 16, 0, 1.0f, 0.0f) == 0xFFFFu, "i32 -1 -> 0xFFFF");
    /* float path */
    CHECK(build_one(HEAD_VALUE_F32, fbits(2.5f), 0, 8, 0, 1.0f, 0.0f) == 3u, "2.5 rounds away from zero");
    CHECK(build_one(HEAD_VALUE_F32, fbits(-2.5f), 1, 8, 0, 1.0f, 0.0f) == 0xFDu, "-2.5 rounds to -3");
    CHECK(build_one(HEAD_VALUE_F32, fbits(0.49999997f), 0, 8, 0, 1.0f, 0.0f) == 0u, "0.49999997 stays 0");
    CHECK(build_one(HEAD_VALUE_F32, fbits(NAN), 1, 8, 0, 1.0f, 0.0f) == 0u, "NaN -> 0");
    CHECK(build_one(HEAD_VALUE_F32, fbits(INFINITY), 0, 12, 0, 1.0f, 0.0f) == 0xFFFu, "+Inf saturates");
    CHECK(build_one(HEAD_VALUE_F32, fbits(-INFINITY), 1, 12, 0, 1.0f, 0.0f) == 0x800u, "-Inf saturates");
    CHECK(build_one(HEAD_VALUE_F32, fbits(1e30f), 0, 32, 0, 1.0f, 0.0f) == 0xFFFFFFFFu, "1e30 saturates u32");
    CHECK(build_one(HEAD_VALUE_F32, fbits(-1e30f), 1, 32, 0, 1.0f, 0.0f) == 0x80000000u, "-1e30 saturates i32");
    CHECK(build_one(HEAD_VALUE_U32, 100u, 0, 16, 0, 0.5f, 10.0f) == 180u, "scaling (100 - 10) / 0.5");
    CHECK(build_one(HEAD_VALUE_F32, fbits(1.5f), 0, 32, 1, 1.5f, 0.0f) == fbits(1.0f), "float field copies the float bits");
    /* bit identical to the old double implementation over random values, widths and signedness */
    srand(12345);
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        uint32_t r = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ ((uint32_t)rand() << 31);
        float f;
        switch (i % 4) {
        case 0: f = (float)(int32_t)r / 1024.0f; break;
        case 1: f = (float)((int)(r % 2001) - 1000) / 2.0f; break; /* many exact .5 cases */
        case 2: { uint32_t e = r & 0x7F800000u; memcpy(&f, &(uint32_t){ (r & 0x807FFFFFu) | (e > 0x4F000000u ? 0x4F000000u : e) }, 4); break; }
        default: f = (float)(r % 100000) * 0.1f - 5000.0f;
        }
        if (f != f) continue;
        int bits = 1 + (int)(rand() % 32), sgn = rand() & 1;
        uint32_t want = old_encode(f, bits, sgn) & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u));
        uint32_t got = build_one(HEAD_VALUE_F32, fbits(f), sgn, bits, 0, 1.0f, 0.0f) & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u));
        if (want != got) { bad++; if (bad < 5) printf("  mismatch f=%.9g bits=%d sgn=%d want=%x got=%x\n", f, bits, sgn, want, got); }
    }
    CHECK(bad == 0, "float encode bit-identical to the old double implementation");
}

/* ---- decode / stale -------------------------------------------------------------------------------------------------- */
static void test_decode(void)
{
    /* hub route that reaches past the received payload although the frame's min_len says otherwise */
    fresh();
    cfg.channel_count = 2;
    cfg.bus_count = 1;
    cfg.hub_bus = 0;
    cfg.hub_frame_count = 1;
    cfg.hub_frames[0] = (HeadHubFrame_t){ 0x00200000, 0x1FFF0000, 0, 2, 0, 2 };
    cfg.hub_route_count = 2;
    cfg.hub_routes[0] = (HeadHubRoute_t){ 0, 0, 2, 0, 0, 1.0f, 0.0f };
    cfg.hub_routes[1] = (HeadHubRoute_t){ 1, 4, 2, 0, 0, 1.0f, 0.0f }; /* offset 4..5, but only 3 bytes arrive */
    uint8_t d[64];
    memset(d, 0xEE, sizeof d);
    d[0] = 0x34; d[1] = 0x12; d[2] = 0;
    CHECK(HeadDecode_Process(&cfg, &live, &stats, 0, 0x00200000, true, d, 3, 100), "hub frame handled");
    CHECK(live.values[0].u == 0x1234 && live.rx_seen[0] && live.last_rx_ms[0] == 100, "hub route inside len decoded");
    CHECK(live.values[1].u == 0 && !live.rx_seen[1], "hub route past len skipped, no timestamp refresh");

    /* hub frame counter: duplicates are not losses */
    fresh();
    cfg.channel_count = 1;
    cfg.bus_count = 1;
    cfg.hub_bus = 0;
    cfg.hub_frame_count = 1;
    cfg.hub_frames[0] = (HeadHubFrame_t){ 0x00200000, 0x1FFF0000, 0, 1, 0, 1 };
    cfg.hub_route_count = 1;
    cfg.hub_routes[0] = (HeadHubRoute_t){ 0, 0, 1, 0, 0, 1.0f, 0.0f };
    static const uint8_t ctr[] = { 10, 10, 11, 14, 14, 15, 14 };
    for (unsigned i = 0; i < sizeof ctr; i++)
        HeadDecode_Process(&cfg, &live, &stats, 0, 0x00200000u | ((uint32_t)ctr[i] << 8), true, d, 1, i);
    CHECK(stats.hub_lost[0] == 2 + 254, "hub loss: 10,10,11,14,14,15,14 -> 2 lost + 14<-15 wrap counts 254");

    /* rx frame with a payload shorter than the signals need */
    fresh();
    cfg.channel_count = 3;
    cfg.bus_count = 2;
    cfg.rx_frame_count = 1;
    cfg.rx_frames[0] = (HeadRxFrame_t){ 1, 0, 8, 0, 0x123, 0x7FF, 0, 0, 3, 0, 0 };
    cfg.rx_signal_count = 3;
    cfg.rx_signals[0] = (HeadRxSignal_t){ 0, 0, 16, 0, 0, 0, -1, 1.0f, 0.0f };
    cfg.rx_signals[1] = (HeadRxSignal_t){ 1, 24, 8, 0, 0, 0, -1, 1.0f, 0.0f };
    cfg.rx_signals[2] = (HeadRxSignal_t){ 2, 39, 16, 1, 0, 0, -1, 1.0f, 0.0f }; /* Motorola, bytes 4-5 */
    memset(d, 0xEE, sizeof d);
    d[0] = 1; d[1] = 2; d[2] = 0; d[3] = 0x55;
    CHECK(HeadDecode_Process(&cfg, &live, &stats, 1, 0x123, false, d, 4, 7), "short rx frame still handled");
    CHECK(live.rx_seen[0] && live.rx_seen[1] && !live.rx_seen[2], "signals past the payload are skipped");
    CHECK(live.values[2].u == 0, "skipped signal keeps its value");
    CHECK(!HeadDecode_Process(&cfg, &live, &stats, 1, 0x123, false, d, 1, 8), "frame losing every signal counts as dropped");
    CHECK(live.last_rx_ms[1] == 7, "no timestamp refresh for skipped signals");
    CHECK(HeadDecode_BitsInRange(8, 39, 16, true) && !HeadDecode_BitsInRange(5, 39, 16, true) && HeadDecode_BitsInRange(4, 24, 8, false) &&
              !HeadDecode_BitsInRange(3, 24, 8, false),
          "BitsInRange");

    /* stale logic */
    fresh();
    cfg.channel_count = 4;
    cfg.channels[0].timeout_ms = 100;
    cfg.channels[1].timeout_ms = 0;
    cfg.channels[2].timeout_ms = 100;
    cfg.channels[3].timeout_ms = 100;
    live.rx_seen[0] = live.rx_seen[1] = live.rx_seen[3] = 1;
    live.last_rx_ms[0] = 1000; live.last_rx_ms[1] = 1000; live.last_rx_ms[3] = 0xFFFFFFF0u;
    HeadDecode_UpdateStale(&cfg, &live, 1100);
    CHECK(!live.stale[0], "age == timeout is fresh");
    HeadDecode_UpdateStale(&cfg, &live, 1101);
    CHECK(live.stale[0], "age > timeout is stale");
    HeadDecode_UpdateStale(&cfg, &live, 999);
    CHECK(!live.stale[0], "timestamp newer than now (ISR race) is fresh, not 4e9 ms old");
    HeadDecode_UpdateStale(&cfg, &live, 1000u + 5000u);
    CHECK(live.stale[0] && !live.stale[1], "timeout 0 never goes stale once seen");
    CHECK(live.stale[2], "never received = stale");
    HeadDecode_UpdateStale(&cfg, &live, 0x10);
    CHECK(!live.stale[3], "age across the 32-bit tick wrap");
    live.last_rx_ms[0] = 1000;
    HeadDecode_UpdateStale(&cfg, &live, 1000u - 1000u);
    CHECK(!live.stale[0], "future by exactly the allowed window is fresh");
    HeadDecode_UpdateStale(&cfg, &live, 1000u - 1001u);
    CHECK(live.stale[0], "far future (wrapped clock) is stale");
    /* a real tick value of 0xFFFFFFFF is a perfectly good timestamp now */
    live.rx_seen[2] = 1; live.last_rx_ms[2] = 0xFFFFFFFFu;
    HeadDecode_UpdateStale(&cfg, &live, 0xFFFFFFFFu);
    CHECK(!live.stale[2], "timestamp 0xFFFFFFFF is not a sentinel any more");

    /* reception rate: frames per window -> 0.1 Hz, refreshed only when a window closes */
    fresh();
    cfg.channel_count = 2;
    live.rate_window_ms = 5000;
    live.rx_count[0] = 400;  /* 200 Hz over 2 s */
    live.rx_count[1] = 21;   /* 10 Hz, one extra frame in the window */
    HeadDecode_UpdateRates(&live, 5000u + HEAD_RATE_WINDOW_MS - 1u);
    CHECK(live.rate_x10[0] == 0 && live.rate_window_ms == 5000, "rate window not closed yet: nothing changes");
    HeadDecode_UpdateRates(&live, 5000u + HEAD_RATE_WINDOW_MS);
    CHECK(live.rate_x10[0] == 2000 && live.rate_x10[1] == 105 && live.rate_x10[2] == 0, "rates in 0.1 Hz");
    live.rx_count[0] += 200;
    HeadDecode_UpdateRates(&live, 5000u + 2u * HEAD_RATE_WINDOW_MS);
    CHECK(live.rate_x10[0] == 1000 && live.rate_x10[1] == 0, "next window counts only its own frames; a silent channel drops to 0");
    live.rate_window_ms = 0xFFFFF000u;
    live.rx_count[0] = 0xFFFFFFF0u; live.rate_base[0] = 0xFFFFFFF0u;
    live.rx_count[0] += 40;  /* counter wraps inside the window */
    HeadDecode_UpdateRates(&live, 0xFFFFF000u + HEAD_RATE_WINDOW_MS); /* clock wraps too */
    CHECK(live.rate_x10[0] == 200, "rate across counter and clock wrap");
}

/* ---- receive ring ---------------------------------------------------------------------------------------------------- */
static void test_rxq(void)
{
    static HeadRxQ_t q;
    HeadRxQ_Init(&q);
    uint8_t d[64];
    memset(d, 7, sizeof d);
    CHECK(HeadRxQ_Peek(&q) == NULL, "empty ring");
    int ok = 0;
    for (uint32_t i = 0; i < 40; i++) {
        d[0] = (uint8_t)i;
        ok += HeadRxQ_Push(&q, 1, 0x100 + i, HEAD_RXQ_FLAG_EXT, 1000 + i, d, 8);
    }
    CHECK(ok == 32 && q.overrun == 8 && q.high == 32, "overrun counted when full");
    for (uint32_t i = 0; i < 32; i++) {
        const HeadRxEntry_t *e = HeadRxQ_Peek(&q);
        CHECK(e && e->id == 0x100 + i && e->data[0] == i && e->ts_ms == 1000 + i && e->bus == 1 && e->len == 8, "fifo order");
        HeadRxQ_Pop(&q);
    }
    CHECK(HeadRxQ_Peek(&q) == NULL, "drained");
    CHECK(HeadRxQ_Push(&q, 0, 1, 0, 0, d, 200) && HeadRxQ_Peek(&q)->len == 64, "length clamped to 64");
    HeadRxQ_Flush(&q);
    CHECK(HeadRxQ_Peek(&q) == NULL, "flush");
    /* counters wrap-free for many pushes/pops */
    for (uint32_t i = 0; i < 1000; i++) {
        CHECK(HeadRxQ_Push(&q, 0, i, 0, i, d, 1), "push after pop");
        CHECK(HeadRxQ_Peek(&q)->id == i, "id");
        HeadRxQ_Pop(&q);
    }
}

/* ---- hproto ------------------------------------------------------------------------------------------------------------ */
static uint8_t ops_flags, ops_store_err, ops_rc;
static uint16_t ops_read_max, ops_live_count;
static int ops_commits;

static void o_info(void *c, HprotoInfo_t *i) { (void)c; (void)i; }
static uint8_t o_commit(void *c, const uint8_t *b, uint32_t l, uint8_t f, uint64_t *h, uint8_t *ap, uint8_t *st, uint8_t *se)
{
    (void)c; (void)b; (void)l;
    ops_commits++;
    ops_flags = f;
    *h = 0x1122334455667788ull;
    *ap = 1;
    *st = 0;
    *se = ops_store_err;
    return ops_rc;
}
static uint8_t o_read(void *c, uint8_t s, uint32_t off, uint8_t *dst, uint16_t max, uint32_t *t, uint16_t *n)
{
    (void)c; (void)s; (void)off;
    ops_read_max = max;
    *t = 1000;
    *n = max;
    memset(dst, 0x5A, max);
    return 0;
}
static uint8_t o_clear(void *c, uint8_t f) { (void)c; (void)f; return 0; }
static uint16_t o_live(void *c, uint16_t first, uint16_t count, uint8_t *dst) { (void)c; (void)first; ops_live_count = count; memset(dst, 0, HPROTO_LIVE_ENTRY * count); return count; }
static uint8_t o_sniff(void *c, uint8_t b, uint8_t e) { (void)c; (void)b; (void)e; return 0; }
static void o_reboot(void *c) { (void)c; }
static const HprotoOps_t ops = { o_info, o_commit, o_read, o_clear, o_live, NULL, o_sniff, NULL, o_reboot };

static size_t call(Hproto_t *p, uint8_t type, const uint8_t *pl, size_t n, uint8_t *resp, size_t cap)
{
    uint8_t msg[300];
    msg[0] = type; msg[1] = 9;
    memcpy(msg + 2, pl, n);
    return Hproto_Handle(p, msg, 2 + n, resp, cap);
}

static void test_hproto(void)
{
    static uint8_t stage[HEAD_MAX_BLOB];
    Hproto_t p;
    uint8_t resp[HFRAME_MAX_BODY], pl[260];
    uint8_t blob[100];
    for (unsigned i = 0; i < sizeof blob; i++) blob[i] = (uint8_t)(i * 7 + 1);
    Hproto_Init(&p, &ops, NULL, stage, sizeof stage);

    /* upload with a retransmitted chunk */
    HeadPutLe32(pl, sizeof blob);
    HeadPutLe32(pl + 4, HeadCfg_Crc32(0, blob, sizeof blob));
    CHECK(call(&p, HMSG_CFG_BEGIN, pl, 8, resp, sizeof resp) == 4 && resp[0] == (HMSG_CFG_BEGIN | 0x80), "begin");
    HeadPutLe32(pl, 0); memcpy(pl + 4, blob, 40);
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 44, resp, sizeof resp) == 6 && HeadLe32(resp + 2) == 40, "chunk 0");
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 44, resp, sizeof resp) == 6 && resp[0] == (HMSG_CFG_CHUNK | 0x80) && HeadLe32(resp + 2) == 40,
          "retransmit of the last chunk is re-acknowledged");
    HeadPutLe32(pl, 40); memcpy(pl + 4, blob + 40, 40);
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 44, resp, sizeof resp) == 6 && HeadLe32(resp + 2) == 80, "chunk 1");
    HeadPutLe32(pl, 0); memcpy(pl + 4, blob, 40);
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 44, resp, sizeof resp) == 4 && resp[0] == HMSG_ERROR && resp[3] == HERR_RANGE, "older chunk -> RANGE");
    HeadPutLe32(pl, 40); memcpy(pl + 4, blob + 40, 40);
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 44, resp, sizeof resp) == 6 && HeadLe32(resp + 2) == 80, "retransmit of chunk 1 still acked");
    HeadPutLe32(pl, 40); memcpy(pl + 4, blob + 40, 30); /* same offset, other length */
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 34, resp, sizeof resp) == 4 && resp[3] == HERR_RANGE, "same offset, different length -> RANGE");
    HeadPutLe32(pl, 90); memcpy(pl + 4, blob + 90, 10);
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 14, resp, sizeof resp) == 4 && resp[3] == HERR_RANGE, "gap -> RANGE");
    HeadPutLe32(pl, 80); memcpy(pl + 4, blob + 80, 20);
    CHECK(call(&p, HMSG_CFG_CHUNK, pl, 24, resp, sizeof resp) == 6 && HeadLe32(resp + 2) == 100, "chunk 2");

    /* COMMIT flag validation leaves the upload staged */
    pl[0] = 0;
    CHECK(call(&p, HMSG_CFG_COMMIT, pl, 1, resp, sizeof resp) == 4 && resp[3] == HERR_BAD_REQUEST, "commit flags 0 refused");
    pl[0] = 0x04;
    CHECK(call(&p, HMSG_CFG_COMMIT, pl, 1, resp, sizeof resp) == 4 && resp[3] == HERR_BAD_REQUEST, "unknown flag refused");
    pl[0] = 0x07;
    CHECK(call(&p, HMSG_CFG_COMMIT, pl, 1, resp, sizeof resp) == 4 && resp[3] == HERR_BAD_REQUEST && ops_commits == 0, "mixed flags refused");
    ops_store_err = HERR_FLASH;
    pl[0] = HPROTO_COMMIT_APPLY | HPROTO_COMMIT_STORE;
    size_t n = call(&p, HMSG_CFG_COMMIT, pl, 1, resp, sizeof resp);
    CHECK(n == 2 + HPROTO_COMMIT_REPLY_LEN && resp[0] == (HMSG_CFG_COMMIT | 0x80) && ops_commits == 1, "commit accepted");
    CHECK(resp[2] == 1 && resp[3] == 0 && HeadLe64(resp + 4) == 0x1122334455667788ull && resp[12] == HERR_FLASH,
          "commit reply: applied, stored, hash, store_err");
    ops_store_err = 0;
    ops_rc = HERR_INVALID_CONFIG;
    HeadPutLe32(pl, sizeof blob);
    HeadPutLe32(pl + 4, HeadCfg_Crc32(0, blob, sizeof blob));
    call(&p, HMSG_CFG_BEGIN, pl, 8, resp, sizeof resp);
    HeadPutLe32(pl, 0); memcpy(pl + 4, blob, 100);
    call(&p, HMSG_CFG_CHUNK, pl, 4 + 100, resp, sizeof resp);
    pl[0] = 1;
    CHECK(call(&p, HMSG_CFG_COMMIT, pl, 1, resp, sizeof resp) == 4 && resp[3] == HERR_INVALID_CONFIG, "platform error becomes an error reply");
    ops_rc = 0;

    /* REBOOT needs the magic */
    CHECK(call(&p, HMSG_REBOOT, pl, 0, resp, sizeof resp) == 4 && resp[3] == HERR_BAD_REQUEST && !Hproto_TakeReboot(&p), "reboot without payload refused");
    HeadPutLe32(pl, 0xDEADBEEFu);
    CHECK(call(&p, HMSG_REBOOT, pl, 4, resp, sizeof resp) == 4 && resp[3] == HERR_BAD_REQUEST && !Hproto_TakeReboot(&p), "reboot with wrong magic refused");
    HeadPutLe32(pl, HPROTO_REBOOT_MAGIC);
    CHECK(call(&p, HMSG_REBOOT, pl, 5, resp, sizeof resp) == 4 && !Hproto_TakeReboot(&p), "reboot with extra bytes refused");
    CHECK(call(&p, HMSG_REBOOT, pl, 4, resp, sizeof resp) == 2 && resp[0] == (HMSG_REBOOT | 0x80) && Hproto_TakeReboot(&p), "reboot with the magic");
    CHECK(!Hproto_TakeReboot(&p), "reboot flag is one shot");

    /* reply capacity clamps */
    pl[0] = 0; HeadPutLe32(pl + 1, 0); HeadPutLe16(pl + 5, 232);
    n = call(&p, HMSG_CFG_READ, pl, 7, resp, 64);
    CHECK(ops_read_max == 54 && n == 2u + 8u + 54u, "CFG_READ clamped to the response buffer");
    n = call(&p, HMSG_CFG_READ, pl, 7, resp, sizeof resp);
    CHECK(ops_read_max == HPROTO_READ_MAX && n == 2u + 8u + HPROTO_READ_MAX, "CFG_READ full size with a 256 byte buffer");
    HeadPutLe16(pl, 0); HeadPutLe16(pl + 2, 32);
    n = call(&p, HMSG_LIVE_GET, pl, 4, resp, 64);
    CHECK(ops_live_count == 8 && n == 2u + 4u + 56u, "LIVE_GET clamped to the response buffer");
    n = call(&p, HMSG_LIVE_GET, pl, 4, resp, sizeof resp);
    CHECK(ops_live_count == 32 && n == 2u + 4u + 224u, "LIVE_GET 32 entries");
    HeadPutLe16(pl + 2, 33);
    CHECK(call(&p, HMSG_LIVE_GET, pl, 4, resp, sizeof resp) == 4 && resp[3] == HERR_RANGE, "LIVE_GET above the limit");
    CHECK(call(&p, HMSG_INFO, pl, 0, resp, 63) == 0, "response buffer below the minimum is refused");
}

int main(void)
{
    test_crc();
    test_dlc();
    test_encode();
    test_decode();
    test_rxq();
    test_hproto();
    printf("unit: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
