#include "head_cfg.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static float rdf(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

uint32_t HeadCfg_Crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static HeadCfgStatus_t check_header(const uint8_t *b, uint32_t len)
{
    if (len < HEAD_BLOB_HEADER_LEN)
        return HEAD_CFG_ERR_SHORT;
    if (rd32(b) != HEAD_BLOB_MAGIC)
        return HEAD_CFG_ERR_MAGIC;
    if (rd16(b + 4) != HEAD_BLOB_FORMAT)
        return HEAD_CFG_ERR_VERSION;
    if (rd16(b + 6) != HEAD_BLOB_HEADER_LEN || rd32(b + 8) != len || len > HEAD_MAX_BLOB)
        return HEAD_CFG_ERR_LENGTH;
    return HEAD_CFG_OK;
}

uint64_t HeadCfg_BlobHash(const uint8_t *b, uint32_t len)
{
    return check_header(b, len) == HEAD_CFG_OK ? rd64(b + 16) : 0;
}

/* section element minimum sizes, by id (1..9) */
static const uint8_t MIN_ELEM[10] = { 0, 12, 16, 8, 24, 20, 12, 12, 16, 16 };

HeadCfgStatus_t HeadCfg_Verify(const uint8_t *b, uint32_t len)
{
    HeadCfgStatus_t st = check_header(b, len);
    if (st != HEAD_CFG_OK)
        return st;
    uint32_t crc = HeadCfg_Crc32(0, b, 28);
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    crc = HeadCfg_Crc32(crc, zero, 4);
    crc = HeadCfg_Crc32(crc, b + 32, len - 32);
    return crc == rd32(b + 28) ? HEAD_CFG_OK : HEAD_CFG_ERR_CRC;
}

HeadCfgStatus_t HeadCfg_Parse(const uint8_t *b, uint32_t len, HeadConfig_t *c)
{
    memset(c, 0, sizeof *c);
    c->hub_bus = -1;
    HeadCfgStatus_t st = HeadCfg_Verify(b, len);
    if (st != HEAD_CFG_OK)
        return st;

    c->config_hash = rd64(b + 16);
    uint32_t nsec = rd32(b + 12), off = HEAD_BLOB_HEADER_LEN;
    if (nsec > 64)
        return HEAD_CFG_ERR_SECTION;

    for (uint32_t s = 0; s < nsec; s++) {
        if (off + 8 > len) { st = HEAD_CFG_ERR_SECTION; goto fail; }
        uint16_t id = rd16(b + off), esize = rd16(b + off + 2);
        uint32_t count = rd32(b + off + 4);
        off += 8;
        if (count > 0 && (esize == 0 || count > (len - off) / esize)) { st = HEAD_CFG_ERR_SECTION; goto fail; }
        const uint8_t *p = b + off;
        off += (uint32_t)esize * count;
        if (id < 1 || id > 9)
            continue; /* unknown section: ignore (forward compatible) */
        if (esize < MIN_ELEM[id]) { st = HEAD_CFG_ERR_SECTION; goto fail; }

        switch (id) {
        case 1:
            if (count > HEAD_MAX_CHANNELS) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->channel_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->channels[i] = (HeadChannel_t){ rd16(p), p[2], p[3], rd16(p + 4), rd32(p + 8) };
            break;
        case 2:
            if (count > HEAD_MAX_HUB_FRAMES) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->hub_frame_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->hub_frames[i] = (HeadHubFrame_t){ rd32(p), rd32(p + 4), p[8], p[9], rd16(p + 10), rd16(p + 12) };
            break;
        case 3:
            if (count > HEAD_MAX_HUB_ROUTES) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->hub_route_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->hub_routes[i] = (HeadHubRoute_t){ rd16(p), p[2], p[3], p[4] };
            break;
        case 4:
            if (count > HEAD_MAX_RX_FRAMES) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->rx_frame_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->rx_frames[i] = (HeadRxFrame_t){ p[0], p[1], p[2], p[3], rd32(p + 4), rd32(p + 8), rd32(p + 12),
                                                   rd16(p + 16), rd16(p + 18), p[20], p[21] };
            break;
        case 5:
            if (count > HEAD_MAX_RX_SIGNALS) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->rx_signal_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->rx_signals[i] = (HeadRxSignal_t){ rd16(p), p[2], p[3], p[4], p[5], p[6], (int16_t)rd16(p + 8), rdf(p + 12), rdf(p + 16) };
            break;
        case 6:
            if (count > HEAD_MAX_BUSES) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->bus_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->buses[i] = (HeadBus_t){ p[0], p[1], p[2], rd32(p + 4), rd32(p + 8) };
            break;
        case 7:
            if (count > HEAD_MAX_FILTERS) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->filter_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->filters[i] = (HeadFilter_t){ p[0], p[1], rd32(p + 4), rd32(p + 8) };
            break;
        case 8:
            if (count > HEAD_MAX_TX_FRAMES) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->tx_frame_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->tx_frames[i] = (HeadTxFrame_t){ p[0], p[1], p[2], rd32(p + 4), rd16(p + 8), rd16(p + 10), rd16(p + 12) };
            break;
        case 9:
            if (count > HEAD_MAX_TX_SIGNALS) { st = HEAD_CFG_ERR_LIMIT; goto fail; }
            c->tx_signal_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->tx_signals[i] = (HeadTxSignal_t){ rd16(p), p[2], p[3], p[4], p[5], p[6], rdf(p + 8), rdf(p + 12) };
            break;
        }
    }

    /* cross-reference checks: every index must be in range so the decoder never needs to re-check */
    for (uint32_t i = 0; i < c->bus_count; i++) {
        const HeadBus_t *bus = &c->buses[i];
        if (bus->peripheral < 1 || bus->peripheral > 3 || bus->bitrate == 0 || (bus->fd && bus->data_bitrate == 0)) { st = HEAD_CFG_ERR_RANGE; goto fail; }
        if (bus->role == 1) {
            if (c->hub_bus >= 0) { st = HEAD_CFG_ERR_RANGE; goto fail; }
            c->hub_bus = (int8_t)i;
        }
    }
    for (uint32_t i = 0; i < c->hub_frame_count; i++) {
        const HeadHubFrame_t *f = &c->hub_frames[i];
        if (f->hub > 7 || f->min_len > 64 || (uint32_t)f->first_route + f->route_count > c->hub_route_count) { st = HEAD_CFG_ERR_RANGE; goto fail; }
    }
    for (uint32_t i = 0; i < c->hub_route_count; i++) {
        const HeadHubRoute_t *r = &c->hub_routes[i];
        if (r->slot >= c->channel_count || r->length < 1 || r->length > 4 || (uint32_t)r->offset + r->length > 64) { st = HEAD_CFG_ERR_RANGE; goto fail; }
    }
    for (uint32_t i = 0; i < c->rx_frame_count; i++) {
        const HeadRxFrame_t *f = &c->rx_frames[i];
        if (f->bus >= c->bus_count || f->dlc > 64 || (uint32_t)f->first_signal + f->signal_count > c->rx_signal_count ||
            (f->mux_enabled && (f->mux_bit_length < 1 || f->mux_bit_length > 32 || (uint32_t)f->mux_start_bit + f->mux_bit_length > 8u * 64u))) { st = HEAD_CFG_ERR_RANGE; goto fail; }
    }
    for (uint32_t i = 0; i < c->rx_signal_count; i++) {
        const HeadRxSignal_t *s = &c->rx_signals[i];
        if (s->slot >= c->channel_count || s->bit_length < 1 || s->bit_length > 32 || (s->is_float && s->bit_length != 32)) { st = HEAD_CFG_ERR_RANGE; goto fail; }
    }
    for (uint32_t i = 0; i < c->tx_frame_count; i++) {
        const HeadTxFrame_t *f = &c->tx_frames[i];
        if (f->bus >= c->bus_count || f->dlc < 1 || f->dlc > 64 || f->period_ms == 0 ||
            (uint32_t)f->first_signal + f->signal_count > c->tx_signal_count) { st = HEAD_CFG_ERR_RANGE; goto fail; }
    }
    for (uint32_t i = 0; i < c->tx_signal_count; i++) {
        const HeadTxSignal_t *s = &c->tx_signals[i];
        if (s->slot >= c->channel_count || s->bit_length < 1 || s->bit_length > 32 || (s->is_float && s->bit_length != 32) ||
            !(s->multiplier != 0.0f)) { st = HEAD_CFG_ERR_RANGE; goto fail; }
    }
    for (uint32_t i = 0; i < c->filter_count; i++)
        if (c->filters[i].bus >= c->bus_count) { st = HEAD_CFG_ERR_RANGE; goto fail; }
    return HEAD_CFG_OK;

fail:
    memset(c, 0, sizeof *c);
    c->hub_bus = -1;
    return st;
}

const char *HeadCfg_StatusText(HeadCfgStatus_t s)
{
    static const char *const t[] = { "ok", "blob too short", "bad magic", "unsupported format version", "length mismatch",
                                     "crc mismatch", "malformed section", "exceeds firmware limits", "index out of range" };
    return (unsigned)s < sizeof t / sizeof t[0] ? t[s] : "unknown";
}
