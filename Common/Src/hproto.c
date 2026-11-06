#include "hproto.h"

#include <string.h>

#include "head_cfg.h"

#include "hframe.h"
#include "head_util.h"

#define rd16 HeadLe16
#define rd32 HeadLe32
#define wr16 HeadPutLe16
#define wr32 HeadPutLe32
#define wr64 HeadPutLe64

/* every reply must fit the smallest response buffer and the framing's payload limit */
_Static_assert(HPROTO_MIN_RESP >= 2u + HPROTO_INFO_BASE_LEN, "INFO reply must fit the minimum response buffer");
_Static_assert(8u + HPROTO_READ_MAX <= HFRAME_MAX_PAYLOAD, "CFG_READ reply must fit one frame");
_Static_assert(4u + 5u * HPROTO_LIVE_MAX <= HFRAME_MAX_PAYLOAD, "LIVE_GET reply must fit one frame");
_Static_assert(HPROTO_CHUNK_MAX + 4u <= HFRAME_MAX_PAYLOAD, "CFG_CHUNK request must fit one frame");

void Hproto_Init(Hproto_t *p, const HprotoOps_t *ops, void *ctx, uint8_t *stage, uint32_t stage_cap)
{
    memset(p, 0, sizeof *p);
    p->ops = ops;
    p->ctx = ctx;
    p->stage = stage;
    p->stage_cap = stage_cap;
}

bool Hproto_TakeReboot(Hproto_t *p)
{
    bool r = p->reboot_pending;
    p->reboot_pending = false;
    return r;
}

static size_t reply(uint8_t type, uint8_t seq, uint8_t *resp, size_t n)
{
    resp[0] = (uint8_t)(type | 0x80u);
    resp[1] = seq;
    return 2 + n;
}

static size_t error(uint8_t req_type, uint8_t seq, uint8_t code, uint8_t *resp)
{
    resp[0] = HMSG_ERROR;
    resp[1] = seq;
    resp[2] = req_type;
    resp[3] = code;
    return 4;
}

uint16_t Hproto_EncodeStats(const HeadStats_t *st, uint8_t nbus, uint8_t *dst, uint16_t cap)
{
    if (nbus > HEAD_MAX_BUSES)
        nbus = (uint8_t)HEAD_MAX_BUSES;
    if (cap < HPROTO_STATS_LEN(nbus))
        return 0;
    uint16_t o = 0;
    dst[o++] = nbus;
    for (uint8_t b = 0; b < nbus; b++) {
        const HeadBusStats_t *s = &st->bus[b];
        wr32(dst + o, s->rx_frames);
        wr32(dst + o + 4, s->rx_matched);
        wr32(dst + o + 8, s->rx_dropped);
        wr32(dst + o + 12, s->fifo_lost);
        dst[o + 16] = s->bus_off;
        dst[o + 17] = s->passive;
        o += 18;
    }
    dst[o++] = 8;
    for (int h = 0; h < 8; h++, o += 4)
        wr32(dst + o, st->hub_lost[h]);
    dst[o++] = nbus; /* appended later than the rest: per-bus transmit counters (old hosts ignore trailing bytes) */
    for (uint8_t b = 0; b < nbus; b++, o += 8) {
        wr32(dst + o, st->bus[b].tx_frames);
        wr32(dst + o + 4, st->bus[b].tx_dropped);
    }
    wr32(dst + o, st->rx_nobus); /* appended last: receive path counters, global */
    wr32(dst + o + 4, st->rx_ring_overrun);
    wr32(dst + o + 8, st->rx_ring_high);
    return (uint16_t)(o + 12);
}

size_t Hproto_Handle(Hproto_t *p, const uint8_t *msg, size_t len, uint8_t *resp, size_t cap)
{
    if (len < 2 || cap < HPROTO_MIN_RESP)
        return 0;
    const uint8_t type = msg[0], seq = msg[1];
    const uint8_t *pl = msg + 2;
    const size_t n = len - 2;
    uint8_t *out = resp + 2;
    const HprotoOps_t *ops = p->ops;
    uint8_t rc;

    switch (type) {
    case HMSG_INFO: {
        HprotoInfo_t i;
        memset(&i, 0, sizeof i);
        ops->get_info(p->ctx, &i);
        out[0] = HPROTO_VERSION;
        out[1] = (uint8_t)HEAD_BLOB_FORMAT;
        wr16(out + 2, i.fw_version);
        wr32(out + 4, i.uptime_ms);
        wr64(out + 8, i.active_hash);
        wr64(out + 16, i.stored_hash);
        out[24] = i.active_source;
        out[25] = i.cm4_alive;
        wr16(out + 26, HEAD_MAX_CHANNELS);
        wr16(out + 28, (uint16_t)HEAD_MAX_BLOB);
        wr16(out + 30, i.channel_count);
        memcpy(out + 32, i.uid, 12);
        wr32(out + 44, i.fdcan_kernel_hz);
        if (cap < 2u + HPROTO_INFO_LEN) /* tiny response buffer: the first 48 bytes only */
            return reply(type, seq, resp, HPROTO_INFO_BASE_LEN);
        /* appended (offsets of everything above stay as they are) */
        out[48] = i.cm4_status;
        out[49] = i.boot_apply_rc;
        wr16(out + 50, i.boot_apply_tries);
        wr32(out + 52, i.hframe_crc_fail);
        wr32(out + 56, i.hframe_overflow);
        wr32(out + 60, i.hframe_desync);
        wr32(out + 64, i.rx_ring_overflow);
        wr32(out + 68, i.uart_err);
        wr32(out + 72, i.uart_tx_fail);
        out[76] = i.tcan_init_status;
        memset(out + 77, 0, 3); /* reserved */
        return reply(type, seq, resp, HPROTO_INFO_LEN);
    }
    case HMSG_IDENT: { /* same layout on the hub: u8 device type, u8 proto, u16 fw, u8 instance (0xFF none), u8 rsvd, u8 uid[12] */
        HprotoInfo_t i;
        memset(&i, 0, sizeof i);
        ops->get_info(p->ctx, &i);
        out[0] = HPROTO_DEVICE_HEAD;
        out[1] = HPROTO_VERSION;
        wr16(out + 2, i.fw_version);
        out[4] = 0xFF;
        out[5] = 0;
        memcpy(out + 6, i.uid, 12);
        return reply(type, seq, resp, 18);
    }
    case HMSG_CFG_BEGIN: {
        if (n != 8)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        uint32_t total = rd32(pl);
        if (total < HEAD_BLOB_HEADER_LEN || total > p->stage_cap || total > HEAD_MAX_BLOB) {
            p->stage_active = false;
            return error(type, seq, HERR_TOO_BIG, resp);
        }
        p->stage_total = total;
        p->stage_crc = rd32(pl + 4);
        p->stage_received = 0;
        p->stage_have_last = false;
        p->stage_active = true;
        wr16(out, HPROTO_CHUNK_MAX);
        return reply(type, seq, resp, 2);
    }
    case HMSG_CFG_CHUNK: {
        if (n < 5)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        if (!p->stage_active)
            return error(type, seq, HERR_BUSY, resp);
        uint32_t off = rd32(pl), cnt = (uint32_t)n - 4;
        if (p->stage_have_last && off == p->stage_last_off && cnt == p->stage_last_len && off + cnt == p->stage_received) {
            wr32(out, p->stage_received); /* retransmit of the chunk just acknowledged (the ack was lost): acknowledge again */
            return reply(type, seq, resp, 4);
        }
        if (off != p->stage_received || cnt > HPROTO_CHUNK_MAX)
            return error(type, seq, HERR_RANGE, resp);
        if (cnt > p->stage_total - p->stage_received)
            return error(type, seq, HERR_TOO_BIG, resp);
        memcpy(p->stage + off, pl + 4, cnt);
        p->stage_received += cnt;
        p->stage_last_off = off;
        p->stage_last_len = cnt;
        p->stage_have_last = true;
        wr32(out, p->stage_received);
        return reply(type, seq, resp, 4);
    }
    case HMSG_CFG_COMMIT: {
        if (n != 1 || pl[0] == 0 || (pl[0] & ~HPROTO_COMMIT_FLAGS_MASK))
            return error(type, seq, HERR_BAD_REQUEST, resp); /* nothing to do / unknown flags: the upload stays staged */
        if (!p->stage_active || p->stage_received != p->stage_total)
            return error(type, seq, HERR_BUSY, resp);
        if (HeadCfg_Crc32(0, p->stage, p->stage_total) != p->stage_crc)
            return error(type, seq, HERR_CRC, resp);
        uint64_t hash = 0;
        uint8_t applied = 0, stored = 0, store_err = HERR_NONE;
        rc = ops->cfg_commit(p->ctx, p->stage, p->stage_total, pl[0], &hash, &applied, &stored, &store_err);
        p->stage_active = false;
        if (rc)
            return error(type, seq, rc, resp);
        out[0] = applied;
        out[1] = stored;
        wr64(out + 2, hash);
        out[10] = store_err;
        return reply(type, seq, resp, HPROTO_COMMIT_REPLY_LEN);
    }
    case HMSG_CFG_READ: {
        if (n != 7)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        uint16_t max = rd16(pl + 5), got = 0;
        uint32_t total = 0, off = rd32(pl + 1);
        if (max > HPROTO_READ_MAX)
            max = HPROTO_READ_MAX;
        if (max > cap - 10u) /* 2 header + 8 (total, offset) */
            max = (uint16_t)(cap - 10u);
        rc = ops->cfg_read(p->ctx, pl[0], off, out + 8, max, &total, &got);
        if (rc)
            return error(type, seq, rc, resp);
        wr32(out, total);
        wr32(out + 4, off);
        return reply(type, seq, resp, 8u + got);
    }
    case HMSG_CFG_CLEAR:
        if (n != 1)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        rc = ops->cfg_clear(p->ctx, pl[0]);
        return rc ? error(type, seq, rc, resp) : reply(type, seq, resp, 0);
    case HMSG_LIVE_GET: {
        if (n != 4)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        uint16_t first = rd16(pl), count = rd16(pl + 2);
        if (count > HPROTO_LIVE_MAX)
            return error(type, seq, HERR_RANGE, resp);
        if (count > (cap - 6u) / 5u) /* 2 header + 4 (first, count) + 5 per entry: a small buffer returns fewer entries */
            count = (uint16_t)((cap - 6u) / 5u);
        uint16_t w = ops->live_get(p->ctx, first, count, out + 4);
        wr16(out, first);
        wr16(out + 2, w);
        return reply(type, seq, resp, 4u + 5u * w);
    }
    case HMSG_STATS_GET: {
        uint16_t w = ops->stats_get ? ops->stats_get(p->ctx, out, (uint16_t)(cap - 2)) : 0;
        return w ? reply(type, seq, resp, w) : error(type, seq, HERR_UNSUPPORTED, resp);
    }
    case HMSG_SNIFF_SET:
        if (n != 2)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        rc = ops->sniff_set ? ops->sniff_set(p->ctx, pl[0], pl[1]) : HERR_UNSUPPORTED;
        return rc ? error(type, seq, rc, resp) : reply(type, seq, resp, 0);
    case HMSG_HUB_RELAY: {
        if (n < 5 || n - 3u > HPROTO_RELAY_MAX_REQ)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        if (!ops->hub_relay)
            return error(type, seq, HERR_UNSUPPORTED, resp);
        if (cap < 4u)
            return error(type, seq, HERR_RANGE, resp);
        uint16_t rl = 0;
        rc = ops->hub_relay(p->ctx, pl[0], rd16(pl + 1), pl + 3, (uint16_t)(n - 3u), out + 1, (uint16_t)(cap - 3u), &rl);
        if (rc)
            return error(type, seq, rc, resp);
        if (rl > cap - 3u)
            return error(type, seq, HERR_RANGE, resp); /* never trust the platform with the reply size */
        out[0] = pl[0];
        return reply(type, seq, resp, 1u + rl);
    }
    case HMSG_REBOOT:
        if (n != 4 || rd32(pl) != HPROTO_REBOOT_MAGIC)
            return error(type, seq, HERR_BAD_REQUEST, resp); /* a stray or malformed message must never reset the module */
        p->reboot_pending = true;
        return reply(type, seq, resp, 0);
    default:
        return error(type, seq, HERR_UNSUPPORTED, resp);
    }
}
