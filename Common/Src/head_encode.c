#include "head_encode.h"

#include <string.h>

void HeadTx_Reset(HeadTxState_t *st)
{
    memset(st, 0, sizeof *st);
}

void HeadTx_InsertBits(uint8_t *d, uint8_t len, uint16_t start, uint8_t n, bool big_endian, uint32_t raw)
{
    uint16_t pos = start;
    for (uint8_t k = 0; k < n; k++) {
        uint32_t bit = big_endian ? (raw >> (n - 1u - k)) & 1u : (raw >> k) & 1u;
        if ((uint16_t)(pos >> 3) < len)
            d[pos >> 3] = (uint8_t)((d[pos >> 3] & ~(1u << (pos & 7))) | (bit << (pos & 7)));
        pos = HeadBits_Next(pos, big_endian);
    }
}

static float channel_as_float(const HeadConfig_t *cfg, const HeadLive_t *live, uint16_t slot)
{
    HeadValue_t v;
    v.u = live->values[slot].u;
    switch (cfg->channels[slot].value_type) {
    case HEAD_VALUE_I32: return (float)v.i;
    case HEAD_VALUE_F32: return v.f;
    default: return (float)v.u;
    }
}

/* field limits as 64-bit integers */
static void field_limits(const HeadTxSignal_t *sg, int64_t *lo, int64_t *hi)
{
    if (sg->is_signed) {
        *hi = (int64_t)((1ull << (sg->bit_length - 1)) - 1ull);
        *lo = -(int64_t)(1ull << (sg->bit_length - 1));
    } else {
        *lo = 0;
        *hi = (int64_t)((1ull << sg->bit_length) - 1ull);
    }
}

static uint32_t saturate_to_field(const HeadTxSignal_t *sg, int64_t x)
{
    int64_t lo, hi;
    field_limits(sg, &lo, &hi);
    x = x < lo ? lo : x > hi ? hi : x;
    return (uint32_t)x; /* low bits of a negative are its two's complement */
}

/* round half away from zero, then saturate (matches rxdecode.encode_tx_signal); float only, no double */
static uint32_t encode_float_to_field(const HeadTxSignal_t *sg, float scaled)
{
    const float lim = 1099511627776.0f; /* 2^40: far beyond any 32-bit field, keeps the int64 conversion exact and in range */
    if (!(scaled == scaled))
        return 0u; /* NaN */
    if (scaled > lim) scaled = lim;
    if (scaled < -lim) scaled = -lim;
    int64_t t = (int64_t)scaled; /* truncates toward zero; exactly representable in float again */
    float frac = scaled - (float)t; /* exact */
    if (frac >= 0.5f)
        t++;
    else if (frac <= -0.5f)
        t--;
    return saturate_to_field(sg, t);
}

uint8_t HeadTx_Build(const HeadConfig_t *cfg, const HeadLive_t *live, uint16_t idx, uint8_t *out)
{
    const HeadTxFrame_t *f = &cfg->tx_frames[idx];
    memset(out, 0, f->dlc);
    for (uint16_t s = 0; s < f->signal_count; s++) {
        const HeadTxSignal_t *sg = &cfg->tx_signals[f->first_signal + s];
        const uint8_t vt = cfg->channels[sg->slot].value_type;
        uint32_t raw;
        if (!sg->is_float && sg->multiplier == 1.0f && sg->offset == 0.0f && vt != HEAD_VALUE_F32) {
            /* exact path: a float would quantise 32-bit counters above 2^24 */
            HeadValue_t v;
            v.u = live->values[sg->slot].u;
            raw = saturate_to_field(sg, vt == HEAD_VALUE_I32 ? (int64_t)v.i : (int64_t)v.u);
        } else {
            float scaled = (channel_as_float(cfg, live, sg->slot) - sg->offset) / sg->multiplier;
            if (sg->is_float)
                memcpy(&raw, &scaled, sizeof raw);
            else
                raw = encode_float_to_field(sg, scaled);
        }
        HeadTx_InsertBits(out, f->dlc, sg->start_bit, sg->bit_length, sg->big_endian, raw);
    }
    return f->dlc;
}

void HeadTx_Poll(const HeadConfig_t *cfg, const HeadLive_t *live, HeadTxState_t *st, HeadStats_t *stats, uint32_t now_ms,
                 HeadTxSendFn send, void *ctx)
{
    if (!st->started) { /* first call after a (re)configuration: stagger the frames by index */
        for (uint16_t i = 0; i < cfg->tx_frame_count; i++)
            st->next_ms[i] = now_ms + (uint32_t)(i % cfg->tx_frames[i].period_ms);
        st->started = 1;
    }
    for (uint16_t i = 0; i < cfg->tx_frame_count; i++) {
        const HeadTxFrame_t *f = &cfg->tx_frames[i];
        if ((int32_t)(now_ms - st->next_ms[i]) < 0)
            continue;
        uint8_t data[HEAD_MAX_FRAME_BYTES];
        uint8_t len = HeadTx_Build(cfg, live, i, data);
        if (send(ctx, f->bus, f->can_id, f->extended != 0, data, len)) {
            if (stats) stats->bus[f->bus].tx_frames++;
            st->next_ms[i] += f->period_ms;
        } else {
            /* bus busy: keep the phase and retry on the next pass; give up on this slot once the next period starts */
            if ((uint32_t)(now_ms - st->next_ms[i]) < f->period_ms)
                continue;
            if (stats) stats->bus[f->bus].tx_dropped++;
            st->next_ms[i] += f->period_ms;
        }
        if ((int32_t)(now_ms - st->next_ms[i]) >= 0) /* fell behind (long stall): resynchronise instead of bursting */
            st->next_ms[i] = now_ms + f->period_ms;
    }
}
