#include "head_decode.h"

#include <string.h>

void HeadDecode_Reset(HeadLive_t *live, HeadStats_t *stats)
{
    for (uint32_t i = 0; i < HEAD_MAX_CHANNELS; i++) {
        live->values[i].u = 0;
        live->last_rx_ms[i] = 0;
        live->rx_seen[i] = 0;
        live->stale[i] = 1;
        live->rx_count[i] = 0;
        live->rate_x10[i] = 0;
        live->rate_base[i] = 0;
    }
    live->rate_window_ms = 0;
    if (stats) {
        memset((void *)stats, 0, sizeof *stats);
        for (uint32_t i = 0; i < HEAD_MAX_HUBS; i++)
            stats->hub_last_counter[i] = -1;
    }
}

uint32_t HeadDecode_ExtractBits(const uint8_t *d, uint8_t len, uint16_t start, uint8_t n, bool big_endian)
{
    uint32_t v = 0;
    uint16_t pos = start;
    for (uint8_t k = 0; k < n; k++) {
        uint32_t bit = (uint16_t)(pos >> 3) < len ? (uint32_t)((d[pos >> 3] >> (pos & 7)) & 1u) : 0u;
        if (big_endian)
            v = (v << 1) | bit;
        else
            v |= bit << k;
        pos = HeadBits_Next(pos, big_endian);
    }
    return v;
}

bool HeadDecode_BitsInRange(uint8_t len, uint16_t start, uint8_t n, bool big_endian)
{
    uint16_t pos = start;
    for (uint8_t k = 0; k < n; k++) {
        if ((uint16_t)(pos >> 3) >= len)
            return false;
        pos = HeadBits_Next(pos, big_endian);
    }
    return true;
}

static void mark_rx(HeadLive_t *live, uint16_t slot, uint32_t now_ms)
{
    live->last_rx_ms[slot] = now_ms;
    live->rx_seen[slot] = 1;
    live->rx_count[slot]++;
}

static bool process_hub(const HeadConfig_t *cfg, HeadLive_t *live, HeadStats_t *stats, uint32_t can_id,
                        const uint8_t *data, uint8_t len, uint32_t now_ms)
{
    bool handled = false;
    for (uint16_t i = 0; i < cfg->hub_frame_count; i++) {
        const HeadHubFrame_t *f = &cfg->hub_frames[i];
        if ((can_id & f->id_mask) != f->id_base || len < f->min_len)
            continue;
        for (uint16_t r = 0; r < f->route_count; r++) {
            const HeadHubRoute_t *rt = &cfg->hub_routes[f->first_route + r];
            if ((uint32_t)rt->offset + rt->length > len)
                continue; /* never read past the received payload (Parse already guarantees offset + length <= min_len) */
            uint32_t v = 0;
            for (uint8_t b = 0; b < rt->length; b++)
                v |= (uint32_t)data[rt->offset + b] << (8 * b);
            if (rt->is_signed && rt->length < 4 && (v & (1u << (8 * rt->length - 1))))
                v |= ~0u << (8 * rt->length);
            if ((rt->flags & HEAD_ROUTE_NO_DATA) && (int32_t)v == HEAD_NO_DATA_CODE)
                continue; /* the hub had no fresh sample: keep the old value, do not refresh the timestamp (the channel goes stale) */
            if (rt->flags & HEAD_ROUTE_SCALED)
                live->values[rt->slot].f = (rt->is_signed ? (float)(int32_t)v : (float)v) * rt->multiplier + rt->value_offset;
            else
                live->values[rt->slot].u = v;
            mark_rx(live, rt->slot, now_ms);
        }
        if (stats && !handled) { /* frame counter: ID bits 15..8, one 8-bit counter per hub that steps once per transmitted frame */
            uint8_t hub = f->hub;
            int16_t counter = (int16_t)((can_id >> 8) & 0xFF);
            int16_t last = stats->hub_last_counter[hub];
            if (last >= 0) {
                uint32_t delta = (uint32_t)(counter - last) & 0xFFu;
                if (delta != 0) /* delta 0 = the same counter again (duplicate / retransmit): nothing was lost */
                    stats->hub_lost[hub] += delta - 1u;
            }
            stats->hub_last_counter[hub] = counter;
        }
        handled = true;
    }
    return handled;
}

static bool process_rx(const HeadConfig_t *cfg, HeadLive_t *live, uint8_t bus, uint32_t can_id, bool extended,
                       const uint8_t *data, uint8_t len, uint32_t now_ms)
{
    bool handled = false;
    for (uint16_t i = 0; i < cfg->rx_frame_count; i++) {
        const HeadRxFrame_t *f = &cfg->rx_frames[i];
        if (f->bus != bus || (bool)f->extended != extended || (can_id & f->id_mask) != (f->can_id & f->id_mask))
            continue;
        int32_t mux = -1;
        if (f->mux_enabled) {
            if (!HeadDecode_BitsInRange(len, f->mux_start_bit, f->mux_bit_length, false))
                continue; /* too short to even hold the multiplexer: not a usable frame */
            mux = (int32_t)HeadDecode_ExtractBits(data, len, f->mux_start_bit, f->mux_bit_length, false);
        }
        uint16_t decoded = 0, skipped = 0;
        for (uint16_t s = 0; s < f->signal_count; s++) {
            const HeadRxSignal_t *sg = &cfg->rx_signals[f->first_signal + s];
            if (sg->mux_value >= 0 && sg->mux_value != mux)
                continue;
            if (!HeadDecode_BitsInRange(len, sg->start_bit, sg->bit_length, sg->big_endian != 0)) {
                skipped++; /* short frame: do not decode zeros into the channel or refresh its timestamp */
                continue;
            }
            uint32_t raw = HeadDecode_ExtractBits(data, len, sg->start_bit, sg->bit_length, sg->big_endian);
            float v;
            if (sg->is_float) {
                memcpy(&v, &raw, sizeof v);
            } else if (sg->is_signed) {
                if (sg->bit_length < 32 && (raw & (1u << (sg->bit_length - 1))))
                    raw |= ~0u << sg->bit_length;
                v = (float)(int32_t)raw;
            } else {
                v = (float)raw;
            }
            live->values[sg->slot].f = v * sg->multiplier + sg->offset;
            mark_rx(live, sg->slot, now_ms);
            decoded++;
        }
        if (skipped == 0 || decoded > 0) /* a frame that lost every signal to a short payload counts as dropped */
            handled = true;
    }
    return handled;
}

bool HeadDecode_Process(const HeadConfig_t *cfg, HeadLive_t *live, HeadStats_t *stats, uint8_t bus, uint32_t can_id,
                        bool extended, const uint8_t *data, uint8_t len, uint32_t now_ms)
{
    bool handled;
    if (bus < HEAD_MAX_BUSES && stats)
        stats->bus[bus].rx_frames++;
    if ((int8_t)bus == cfg->hub_bus)
        handled = extended && process_hub(cfg, live, stats, can_id, data, len, now_ms);
    else
        handled = process_rx(cfg, live, bus, can_id, extended, data, len, now_ms);
    if (bus < HEAD_MAX_BUSES && stats) {
        if (handled)
            stats->bus[bus].rx_matched++;
        else
            stats->bus[bus].rx_dropped++;
    }
    return handled;
}

void HeadDecode_UpdateStale(const HeadConfig_t *cfg, HeadLive_t *live, uint32_t now_ms)
{
    for (uint16_t i = 0; i < cfg->channel_count; i++) {
        uint32_t timeout = cfg->channels[i].timeout_ms;
        uint8_t stale;
        if (!live->rx_seen[i]) {
            stale = 1;
        } else if (timeout == 0) {
            stale = 0; /* 0 = no timeout: valid forever once received */
        } else {
            int32_t age = (int32_t)(now_ms - live->last_rx_ms[i]); /* negative: stamped after `now` was sampled */
            if (age >= 0)
                stale = (uint32_t)age > timeout;
            else
                stale = age < -(int32_t)HEAD_STALE_FUTURE_MS; /* far in the "future" = the clock wrapped past an old stamp */
        }
        live->stale[i] = stale;
    }
}

void HeadDecode_UpdateRates(HeadLive_t *live, uint32_t now_ms)
{
    uint32_t elapsed = now_ms - live->rate_window_ms;
    if (elapsed < HEAD_RATE_WINDOW_MS)
        return;
    for (uint32_t i = 0; i < HEAD_MAX_CHANNELS; i++) {
        uint32_t c = live->rx_count[i];
        uint64_t r = (uint64_t)(c - live->rate_base[i]) * 10000u / elapsed; /* frames per ms * 10000 = 0.1 Hz */
        live->rate_x10[i] = r > 0xFFFFu ? 0xFFFFu : (uint16_t)r;
        live->rate_base[i] = c;
    }
    live->rate_window_ms = now_ms;
}
