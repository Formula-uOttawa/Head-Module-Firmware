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
        pos = big_endian ? ((pos & 7) ? (uint16_t)(pos - 1) : (uint16_t)(pos + 15)) : (uint16_t)(pos + 1);
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

uint8_t HeadTx_Build(const HeadConfig_t *cfg, const HeadLive_t *live, uint16_t idx, uint8_t *out)
{
    const HeadTxFrame_t *f = &cfg->tx_frames[idx];
    memset(out, 0, f->dlc);
    for (uint16_t s = 0; s < f->signal_count; s++) {
        const HeadTxSignal_t *sg = &cfg->tx_signals[f->first_signal + s];
        float scaled = (channel_as_float(cfg, live, sg->slot) - sg->offset) / sg->multiplier;
        uint32_t raw;
        if (sg->is_float) {
            memcpy(&raw, &scaled, sizeof raw);
        } else {
            /* round half away from zero, saturate to the field (matches rxdecode.encode_tx_signal) */
            double x = scaled >= 0.0f ? (double)scaled + 0.5 : (double)scaled - 0.5;
            double lo, hi;
            if (sg->is_signed) {
                hi = (double)((1ull << (sg->bit_length - 1)) - 1ull);
                lo = -(double)(1ull << (sg->bit_length - 1));
            } else {
                lo = 0.0;
                hi = (double)((1ull << sg->bit_length) - 1ull);
            }
            if (!(x == x)) x = 0.0; /* NaN */
            x = x < lo ? lo : x > hi ? hi : x;
            raw = (uint32_t)(int64_t)x; /* truncation == rounding after the +-0.5 above; low bits of a negative are two's complement */
        }
        HeadTx_InsertBits(out, f->dlc, sg->start_bit, sg->bit_length, sg->big_endian, raw);
    }
    return f->dlc;
}

void HeadTx_Poll(const HeadConfig_t *cfg, const HeadLive_t *live, HeadTxState_t *st, HeadStats_t *stats, uint32_t now_ms,
                 HeadTxSendFn send, void *ctx)
{
    if (!st->started) { /* first call after a (re)configuration: spread nothing, just start now */
        for (uint16_t i = 0; i < cfg->tx_frame_count; i++)
            st->next_ms[i] = now_ms;
        st->started = 1;
    }
    for (uint16_t i = 0; i < cfg->tx_frame_count; i++) {
        const HeadTxFrame_t *f = &cfg->tx_frames[i];
        if ((int32_t)(now_ms - st->next_ms[i]) < 0)
            continue;
        uint8_t data[64];
        uint8_t len = HeadTx_Build(cfg, live, i, data);
        if (send(ctx, f->bus, f->can_id, f->extended != 0, data, len)) {
            if (stats) stats->bus[f->bus].tx_frames++;
            st->next_ms[i] += f->period_ms;
            if ((int32_t)(now_ms - st->next_ms[i]) >= 0) /* fell behind (long stall): resynchronise instead of bursting */
                st->next_ms[i] = now_ms + f->period_ms;
        } else {
            if (stats) stats->bus[f->bus].tx_dropped++;
            st->next_ms[i] = now_ms + 1u; /* retry shortly */
        }
    }
}
