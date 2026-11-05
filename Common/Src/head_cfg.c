#include "head_cfg.h"

#include <string.h>

#include "head_util.h"

#define rd16 HeadLe16
#define rd32 HeadLe32
#define rd64 HeadLe64
static float rdf(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static bool finite_f(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return ((u >> 23) & 0xFFu) != 0xFFu; /* exponent all ones = Inf/NaN */
}

/* standard CRC-32 table (poly 0xEDB88320), one lookup per byte */
static const uint32_t CRC32_TABLE[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

uint32_t HeadCfg_Crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ data[i]) & 0xFFu];
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

/* a filter (mask match) accepts every id of a frame family (id & fmask == base & fmask over ids with (id & mask) == base)
   iff it only looks at bits the family fixes and agrees on them */
static bool filter_covers(const HeadConfig_t *c, uint8_t bus, bool ext, uint32_t base, uint32_t mask)
{
    for (uint32_t i = 0; i < c->filter_count; i++) {
        const HeadFilter_t *f = &c->filters[i];
        if (f->bus == bus && (f->extended != 0) == ext && (f->mask & ~mask) == 0 && ((f->id ^ base) & f->mask) == 0)
            return true;
    }
    return false;
}

static uint32_t id_max(uint8_t extended) { return extended ? HEAD_EXT_ID_MAX : HEAD_STD_ID_MAX; }

/* cross-reference and range checks: every index must be in range so the decoder never needs to re-check, and nothing
   that cannot be applied to the hardware may pass (a failing apply takes the buses down) */
static HeadCfgStatus_t validate(HeadConfig_t *c)
{
    uint32_t periph_used = 0;
    for (uint32_t i = 0; i < c->channel_count; i++) {
        const HeadChannel_t *ch = &c->channels[i];
        if (ch->value_type > HEAD_VALUE_F32 || ch->source > HEAD_SRC_CAN_RX)
            return HEAD_CFG_ERR_RANGE;
    }
    for (uint32_t i = 0; i < c->bus_count; i++) {
        const HeadBus_t *bus = &c->buses[i];
        if (bus->peripheral < 1 || bus->peripheral > HEAD_MAX_PERIPHERAL || (periph_used & (1u << bus->peripheral)) || bus->fd > 1 ||
            bus->bitrate == 0 || bus->bitrate > HEAD_MAX_NOMINAL_BITRATE ||
            (bus->fd && (bus->data_bitrate == 0 || bus->data_bitrate > HEAD_MAX_DATA_BITRATE)))
            return HEAD_CFG_ERR_RANGE;
        periph_used |= 1u << bus->peripheral;
        if (bus->role == 1) {
            if (c->hub_bus >= 0)
                return HEAD_CFG_ERR_RANGE;
            c->hub_bus = (int8_t)i;
        }
    }
    if (c->hub_frame_count && c->hub_bus < 0)
        return HEAD_CFG_ERR_RANGE; /* hub frames without a hub bus can never match */
    for (uint32_t i = 0; i < c->hub_frame_count; i++) {
        const HeadHubFrame_t *f = &c->hub_frames[i];
        if (f->hub >= HEAD_MAX_HUBS || f->min_len > HEAD_MAX_FRAME_BYTES || (uint32_t)f->first_route + f->route_count > c->hub_route_count ||
            f->id_mask > HEAD_EXT_ID_MAX || (f->id_base & ~f->id_mask) != 0)
            return HEAD_CFG_ERR_RANGE;
        for (uint32_t r = 0; r < f->route_count; r++) { /* a route may only read bytes the frame is guaranteed to have */
            const HeadHubRoute_t *rt = &c->hub_routes[f->first_route + r];
            if ((uint32_t)rt->offset + rt->length > f->min_len)
                return HEAD_CFG_ERR_RANGE;
        }
    }
    for (uint32_t i = 0; i < c->hub_route_count; i++) {
        const HeadHubRoute_t *r = &c->hub_routes[i];
        if (r->slot >= c->channel_count || r->length < 1 || r->length > 4 || (uint32_t)r->offset + r->length > HEAD_MAX_FRAME_BYTES)
            return HEAD_CFG_ERR_RANGE;
    }
    for (uint32_t i = 0; i < c->rx_frame_count; i++) {
        const HeadRxFrame_t *f = &c->rx_frames[i];
        if (f->bus >= c->bus_count || f->extended > 1 || f->mux_enabled > 1 || f->dlc > HEAD_MAX_FRAME_BYTES ||
            f->can_id > id_max(f->extended) || f->id_mask > id_max(f->extended) ||
            (uint32_t)f->first_signal + f->signal_count > c->rx_signal_count ||
            (f->mux_enabled && (f->mux_bit_length < 1 || f->mux_bit_length > 32 ||
                                (uint32_t)f->mux_start_bit + f->mux_bit_length > 8u * HEAD_MAX_FRAME_BYTES)))
            return HEAD_CFG_ERR_RANGE;
    }
    for (uint32_t i = 0; i < c->rx_signal_count; i++) {
        const HeadRxSignal_t *s = &c->rx_signals[i];
        if (s->slot >= c->channel_count || s->bit_length < 1 || s->bit_length > 32 || (s->is_float && s->bit_length != 32) ||
            !finite_f(s->multiplier) || !finite_f(s->offset))
            return HEAD_CFG_ERR_RANGE;
    }
    for (uint32_t i = 0; i < c->tx_frame_count; i++) {
        const HeadTxFrame_t *f = &c->tx_frames[i];
        if (f->bus >= c->bus_count || f->extended > 1 || f->can_id > id_max(f->extended) || f->period_ms == 0 ||
            !HeadDlc_LenIsValid(f->dlc) || f->dlc < 1 || (!c->buses[f->bus].fd && f->dlc > HEAD_CLASSIC_FRAME_BYTES) ||
            (uint32_t)f->first_signal + f->signal_count > c->tx_signal_count)
            return HEAD_CFG_ERR_RANGE;
    }
    for (uint32_t i = 0; i < c->tx_signal_count; i++) {
        const HeadTxSignal_t *s = &c->tx_signals[i];
        if (s->slot >= c->channel_count || s->bit_length < 1 || s->bit_length > 32 || (s->is_float && s->bit_length != 32) ||
            !finite_f(s->multiplier) || !(s->multiplier != 0.0f) || !finite_f(s->offset))
            return HEAD_CFG_ERR_RANGE;
    }
    uint32_t nstd[HEAD_MAX_BUSES] = { 0 }, next[HEAD_MAX_BUSES] = { 0 };
    for (uint32_t i = 0; i < c->filter_count; i++) {
        const HeadFilter_t *f = &c->filters[i];
        if (f->bus >= c->bus_count || f->extended > 1 || f->id > id_max(f->extended) || f->mask > id_max(f->extended))
            return HEAD_CFG_ERR_RANGE;
        if (f->extended ? ++next[f->bus] > HEAD_MAX_EXT_FILTERS_PER_BUS : ++nstd[f->bus] > HEAD_MAX_STD_FILTERS_PER_BUS)
            return HEAD_CFG_ERR_RANGE;
    }
    /* every frame the decoder expects must be able to pass the hardware filters of its bus, otherwise it is silently never received */
    for (uint32_t i = 0; i < c->hub_frame_count; i++) {
        const HeadHubFrame_t *f = &c->hub_frames[i];
        if (!filter_covers(c, (uint8_t)c->hub_bus, true, f->id_base, f->id_mask))
            return HEAD_CFG_ERR_RANGE;
    }
    for (uint32_t i = 0; i < c->rx_frame_count; i++) {
        const HeadRxFrame_t *f = &c->rx_frames[i];
        if (!filter_covers(c, f->bus, f->extended != 0, f->can_id & f->id_mask, f->id_mask))
            return HEAD_CFG_ERR_RANGE;
    }
    return HEAD_CFG_OK;
}

static HeadCfgStatus_t parse_sections(const uint8_t *b, uint32_t len, HeadConfig_t *c)
{
    c->config_hash = rd64(b + 16);
    uint32_t nsec = rd32(b + 12), off = HEAD_BLOB_HEADER_LEN;
    if (nsec > 64)
        return HEAD_CFG_ERR_SECTION;

    for (uint32_t s = 0; s < nsec; s++) {
        if (off + 8 > len) return HEAD_CFG_ERR_SECTION;
        uint16_t id = rd16(b + off), esize = rd16(b + off + 2);
        uint32_t count = rd32(b + off + 4);
        off += 8;
        if (count > 0 && (esize == 0 || count > (len - off) / esize)) return HEAD_CFG_ERR_SECTION;
        const uint8_t *p = b + off;
        off += (uint32_t)esize * count;
        if (id < 1 || id > 9)
            continue; /* unknown section: ignore (forward compatible) */
        if (esize < MIN_ELEM[id]) return HEAD_CFG_ERR_SECTION;

        switch (id) {
        case 1:
            if (count > HEAD_MAX_CHANNELS) return HEAD_CFG_ERR_LIMIT;
            c->channel_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->channels[i] = (HeadChannel_t){ rd16(p), p[2], p[3], rd16(p + 4), rd32(p + 8) };
            break;
        case 2:
            if (count > HEAD_MAX_HUB_FRAMES) return HEAD_CFG_ERR_LIMIT;
            c->hub_frame_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->hub_frames[i] = (HeadHubFrame_t){ rd32(p), rd32(p + 4), p[8], p[9], rd16(p + 10), rd16(p + 12) };
            break;
        case 3:
            if (count > HEAD_MAX_HUB_ROUTES) return HEAD_CFG_ERR_LIMIT;
            c->hub_route_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->hub_routes[i] = (HeadHubRoute_t){ rd16(p), p[2], p[3], p[4] };
            break;
        case 4:
            if (count > HEAD_MAX_RX_FRAMES) return HEAD_CFG_ERR_LIMIT;
            c->rx_frame_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->rx_frames[i] = (HeadRxFrame_t){ p[0], p[1], p[2], p[3], rd32(p + 4), rd32(p + 8), rd32(p + 12),
                                                   rd16(p + 16), rd16(p + 18), p[20], p[21] };
            break;
        case 5:
            if (count > HEAD_MAX_RX_SIGNALS) return HEAD_CFG_ERR_LIMIT;
            c->rx_signal_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->rx_signals[i] = (HeadRxSignal_t){ rd16(p), p[2], p[3], p[4], p[5], p[6], (int16_t)rd16(p + 8), rdf(p + 12), rdf(p + 16) };
            break;
        case 6:
            if (count > HEAD_MAX_BUSES) return HEAD_CFG_ERR_LIMIT;
            c->bus_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->buses[i] = (HeadBus_t){ p[0], p[1], p[2], rd32(p + 4), rd32(p + 8) };
            break;
        case 7:
            if (count > HEAD_MAX_FILTERS) return HEAD_CFG_ERR_LIMIT;
            c->filter_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->filters[i] = (HeadFilter_t){ p[0], p[1], rd32(p + 4), rd32(p + 8) };
            break;
        case 8:
            if (count > HEAD_MAX_TX_FRAMES) return HEAD_CFG_ERR_LIMIT;
            c->tx_frame_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->tx_frames[i] = (HeadTxFrame_t){ p[0], p[1], p[2], rd32(p + 4), rd16(p + 8), rd16(p + 10), rd16(p + 12) };
            break;
        case 9:
            if (count > HEAD_MAX_TX_SIGNALS) return HEAD_CFG_ERR_LIMIT;
            c->tx_signal_count = (uint16_t)count;
            for (uint32_t i = 0; i < count; i++, p += esize)
                c->tx_signals[i] = (HeadTxSignal_t){ rd16(p), p[2], p[3], p[4], p[5], p[6], rdf(p + 8), rdf(p + 12) };
            break;
        }
    }

    return validate(c);
}

static HeadCfgStatus_t parse_checked(const uint8_t *b, uint32_t len, HeadConfig_t *c, bool check_crc)
{
    memset(c, 0, sizeof *c);
    c->hub_bus = -1;
    HeadCfgStatus_t st = check_crc ? HeadCfg_Verify(b, len) : check_header(b, len);
    if (st == HEAD_CFG_OK)
        st = parse_sections(b, len, c);
    if (st != HEAD_CFG_OK) { /* leave nothing half-decoded behind */
        memset(c, 0, sizeof *c);
        c->hub_bus = -1;
    }
    return st;
}

HeadCfgStatus_t HeadCfg_Parse(const uint8_t *b, uint32_t len, HeadConfig_t *c)
{
    return parse_checked(b, len, c, true);
}

HeadCfgStatus_t HeadCfg_ParseVerified(const uint8_t *b, uint32_t len, HeadConfig_t *c)
{
    return parse_checked(b, len, c, false);
}

const char *HeadCfg_StatusText(HeadCfgStatus_t s)
{
    static const char *const t[] = { "ok", "blob too short", "bad magic", "unsupported format version", "length mismatch",
                                     "crc mismatch", "malformed section", "exceeds firmware limits", "index out of range" };
    return (unsigned)s < sizeof t / sizeof t[0] ? t[s] : "unknown";
}
