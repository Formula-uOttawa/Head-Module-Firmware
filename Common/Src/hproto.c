#include "hproto.h"

#include <string.h>

#include "head_cfg.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

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

size_t Hproto_Handle(Hproto_t *p, const uint8_t *msg, size_t len, uint8_t *resp, size_t cap)
{
    if (len < 2 || cap < 64)
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
        return reply(type, seq, resp, 48);
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
        if (off != p->stage_received || cnt > HPROTO_CHUNK_MAX)
            return error(type, seq, HERR_RANGE, resp);
        if (cnt > p->stage_total - p->stage_received)
            return error(type, seq, HERR_TOO_BIG, resp);
        memcpy(p->stage + off, pl + 4, cnt);
        p->stage_received += cnt;
        wr32(out, p->stage_received);
        return reply(type, seq, resp, 4);
    }
    case HMSG_CFG_COMMIT: {
        if (n != 1)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        if (!p->stage_active || p->stage_received != p->stage_total)
            return error(type, seq, HERR_BUSY, resp);
        if (HeadCfg_Crc32(0, p->stage, p->stage_total) != p->stage_crc)
            return error(type, seq, HERR_CRC, resp);
        uint64_t hash = 0;
        uint8_t applied = 0, stored = 0;
        rc = ops->cfg_commit(p->ctx, p->stage, p->stage_total, pl[0], &hash, &applied, &stored);
        p->stage_active = false;
        if (rc)
            return error(type, seq, rc, resp);
        out[0] = applied;
        out[1] = stored;
        wr64(out + 2, hash);
        return reply(type, seq, resp, 10);
    }
    case HMSG_CFG_READ: {
        if (n != 7)
            return error(type, seq, HERR_BAD_REQUEST, resp);
        uint16_t max = rd16(pl + 5), got = 0;
        uint32_t total = 0, off = rd32(pl + 1);
        if (max > 232)
            max = 232;
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
        if (count > 48)
            return error(type, seq, HERR_RANGE, resp);
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
    case HMSG_REBOOT:
        p->reboot_pending = true;
        return reply(type, seq, resp, 0);
    default:
        return error(type, seq, HERR_UNSUPPORTED, resp);
    }
}
