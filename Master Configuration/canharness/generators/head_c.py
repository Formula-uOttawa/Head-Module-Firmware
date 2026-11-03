"""Generate the head-module C sources: routing/decoding tables, RX filter plan, and the decode runtime.

Outputs (all deterministic, identical inputs -> identical files):
  head_config.h              types, counts, externs, runtime API
  head_config_generated.c    channel table, hub frame/route tables, custom RX frame/signal tables, per-bus filters
  head_decode.c              fixed decode/route runtime (Head_Init, Head_ProcessFrame, Head_ChannelIsStale)
"""

from __future__ import annotations

from .. import canid
from ..model import Project, SensorType, timeout_for_rate
from ..packing import pack

HUB_FRAME_MASK = canid.FILTER_MASK
HUB_LEVEL_MASK = 0x1F000000  # type + hub id
HUB_TYPE_MASK = 0x18000000  # message type only
STD_MASK = 0x7FF
EXT_MASK = 0x1FFFFFFF


def _f(x: float) -> str:
    s = f"{x:.9g}"
    return (s if any(c in s for c in ".en") else s + ".0") + "f"


def channel_table(project: Project):
    """Enabled channels sorted by channel_id; the list index is the channel's live-data slot."""
    return sorted((c for c in project.channels if c.enabled), key=lambda c: c.channel_id)


def plan_filters(project: Project) -> dict[str, dict]:
    """Per-bus hardware filter plan: {bus_id: {std: [(id, mask)], ext: [...], std_needed, ext_needed}}."""
    plans = {b.id: {"std": [], "ext": []} for b in project.buses}
    for b in project.buses:
        if b.role == "hub":
            frames, _ = pack(project)
            flt = sorted({(f.id_base, HUB_FRAME_MASK) for f in frames})
            if len(flt) > b.ext_filters:  # widen to one filter per hub, then per message type
                flt = sorted({(f.id_base & HUB_LEVEL_MASK, HUB_LEVEL_MASK) for f in frames})
            if len(flt) > b.ext_filters:
                flt = [(0, HUB_TYPE_MASK)] if frames else []
            plans[b.id]["ext"] = flt
    for rf in project.rx_frames:
        if rf.bus not in plans:
            continue
        mask = rf.id_mask if rf.id_mask is not None else (EXT_MASK if rf.extended else STD_MASK)
        plans[rf.bus]["ext" if rf.extended else "std"].append((rf.can_id & mask, mask))
    for p in plans.values():
        for k in ("std", "ext"):
            p[k] = sorted(set(p[k]))
        p["std_needed"], p["ext_needed"] = len(p["std"]), len(p["ext"])
    return plans


def _arr(name_type: str, name: str, items: list[str], count_macro: str | None = None) -> list[str]:
    n = max(len(items), 1)
    body = items or ["    {0}"]  # C forbids zero-length arrays
    return [f"const {name_type} {name}[{n}] = {{", *body, "};", ""]


def generate_head(project: Project) -> dict[str, str]:
    chans = channel_table(project)
    slot = {c.channel_id: i for i, c in enumerate(chans)}
    frames, _ = pack(project)
    buses = project.buses
    bus_index = {b.id: i for i, b in enumerate(buses)}
    hub_bus = next(i for i, b in enumerate(buses) if b.role == "hub")
    plans = plan_filters(project)

    hub_frames, routes = [], []
    for f in frames:
        first = len(routes)
        for p in f.placements:
            routes.append(f"    {{ {slot[p.channel.channel_id]}, {p.offset}, {p.length}, {int(p.channel.signed)} }},  // {p.channel.short_name}")
        hub_frames.append(f"    {{ 0x{f.id_base:08X}UL, 0x{HUB_FRAME_MASK:08X}UL, {f.hub}, {f.payload_bytes}, {first}, {len(f.placements)} }},"
                          f"  // hub {f.hub} {f.rate_hz} Hz frame {f.frame_idx}")

    rx_frames, rx_signals = [], []
    for rf in project.rx_frames:
        first = len(rx_signals)
        sigs = [s for s in rf.signals if s.channel_id in slot]
        for s in sigs:
            ch = next(c for c in chans if c.channel_id == s.channel_id)
            rx_signals.append(f"    {{ {slot[s.channel_id]}, {s.start_bit}, {s.bit_length}, {int(s.byte_order == 'motorola')}, "
                              f"{int(s.signed)}, {int(s.is_float)}, {-1 if s.mux_value is None else s.mux_value}, {_f(s.multiplier)}, {_f(s.offset)} }},"
                              f"  // {ch.short_name}")
        mask = rf.id_mask if rf.id_mask is not None else (EXT_MASK if rf.extended else STD_MASK)
        mux = rf.mux_start_bit is not None
        rx_frames.append(f"    {{ {bus_index[rf.bus]}, {int(rf.extended)}, {rf.dlc}, {int(mux)}, 0x{rf.can_id:08X}UL, 0x{mask:08X}UL, {rf.timeout_ms}, "
                         f"{first}, {len(sigs)}, {rf.mux_start_bit or 0}, {rf.mux_bit_length or 0} }},  // {rf.name}")

    ch_rows = []
    for c in chans:
        if c.source == "can_rx":
            frame = next((rf for rf in project.rx_frames if any(s.channel_id == c.channel_id for s in rf.signals)), None)
            vt, src, timeout = "HEAD_VALUE_F32", "HEAD_SRC_CAN_RX", frame.timeout_ms if frame else 1000
        else:
            vt, src, timeout = ("HEAD_VALUE_I32" if c.signed else "HEAD_VALUE_U32"), "HEAD_SRC_HUB", timeout_for_rate(c.sample_rate_hz)
        ch_rows.append(f"    {{ {c.channel_id}, {vt}, {src}, {c.sample_rate_hz}, {timeout} }},  // {c.short_name}")

    bus_rows, filt_defs = [], []
    for b in buses:
        pl = plans[b.id]
        for kind in ("std", "ext"):
            filt_defs += _arr("HeadFilter_t", f"HEAD_FILTERS_{b.id}_{kind.upper()}", [f"    {{ 0x{i:08X}UL, 0x{m:08X}UL }}," for i, m in pl[kind]])
        bus_rows.append(f'    {{ "{b.name}", {int(b.type == "can_fd")}, {b.bitrate}, {b.data_bitrate or 0}, "{b.head_peripheral}", '
                        f"HEAD_FILTERS_{b.id}_STD, {pl['std_needed']}, HEAD_FILTERS_{b.id}_EXT, {pl['ext_needed']} }},")

    banner = ["/*", " * AUTO GENERATED by CAN-harness Master Configuration - DO NOT EDIT",
              f" * project '{project.name}'  config hash {project.config_hash()}", " */"]
    h = banner + [
        "#ifndef HEAD_CONFIG_H", "#define HEAD_CONFIG_H", "", "#include <stdint.h>", "#include <stdbool.h>", "",
        f"#define HEAD_CONFIG_HASH 0x{project.config_hash()}ULL",
        f"#define HEAD_CHANNEL_COUNT {len(chans)}", f"#define HEAD_HUB_FRAME_COUNT {len(frames)}",
        f"#define HEAD_HUB_ROUTE_COUNT {len(routes)}", f"#define HEAD_RX_FRAME_COUNT {len(rx_frames)}",
        f"#define HEAD_RX_SIGNAL_COUNT {len(rx_signals)}", f"#define HEAD_BUS_COUNT {len(buses)}",
        f"#define HEAD_HUB_BUS_INDEX {hub_bus}", "",
        "/* Bus indices, and the FDCAN filter elements CubeMX must provide (StdFiltersNbr / ExtFiltersNbr) */",
        *[f"#define HEAD_BUS_{b.id} {i}\n#define HEAD_BUS_{b.id}_STD_FILTERS {plans[b.id]['std_needed']}\n#define HEAD_BUS_{b.id}_EXT_FILTERS {plans[b.id]['ext_needed']}"
          for i, b in enumerate(buses)], "",
        "typedef enum { HEAD_VALUE_U32 = 0, HEAD_VALUE_I32, HEAD_VALUE_F32 } HeadValueType_t;",
        "typedef enum { HEAD_SRC_HUB = 0, HEAD_SRC_CAN_RX } HeadSource_t;",
        "typedef union { uint32_t u; int32_t i; float f; } HeadValue_t;", "",
        "/* index in HEAD_CHANNELS == slot in g_head_values / g_head_last_rx_ms */",
        "typedef struct { uint16_t channel_id; uint8_t value_type; uint8_t source; uint16_t rate_hz; uint32_t timeout_ms; } HeadChannel_t;",
        "/* hub data frame: matched with (id & id_mask) == id_base; payload must be >= min_len bytes */",
        "typedef struct { uint32_t id_base; uint32_t id_mask; uint8_t hub; uint8_t min_len; uint16_t first_route; uint16_t route_count; } HeadHubFrame_t;",
        "typedef struct { uint16_t slot; uint8_t offset; uint8_t length; uint8_t is_signed; } HeadHubRoute_t;",
        "typedef struct { uint8_t bus; uint8_t extended; uint8_t dlc; uint8_t mux_enabled; uint32_t can_id; uint32_t id_mask; uint32_t timeout_ms;",
        "                 uint16_t first_signal; uint16_t signal_count; uint8_t mux_start_bit; uint8_t mux_bit_length; } HeadRxFrame_t;",
        "typedef struct { uint16_t slot; uint8_t start_bit; uint8_t bit_length; uint8_t big_endian; uint8_t is_signed; uint8_t is_float;",
        "                 int16_t mux_value; float multiplier; float offset; } HeadRxSignal_t;",
        "typedef struct { uint32_t id; uint32_t mask; } HeadFilter_t;",
        "typedef struct { const char *name; uint8_t fd; uint32_t bitrate; uint32_t data_bitrate; const char *peripheral;",
        "                 const HeadFilter_t *std_filters; uint8_t std_count; const HeadFilter_t *ext_filters; uint8_t ext_count; } HeadBus_t;", "",
        "extern const HeadChannel_t HEAD_CHANNELS[];", "extern const HeadHubFrame_t HEAD_HUB_FRAMES[];", "extern const HeadHubRoute_t HEAD_HUB_ROUTES[];",
        "extern const HeadRxFrame_t HEAD_RX_FRAMES[];", "extern const HeadRxSignal_t HEAD_RX_SIGNALS[];", "extern const HeadBus_t HEAD_BUSES[];", "",
        "/* ---- runtime (head_decode.c). Call from the core that owns CAN RX. ---- */",
        "extern volatile HeadValue_t g_head_values[];       /* latest value per channel slot (type per HEAD_CHANNELS[].value_type) */",
        "extern volatile uint32_t g_head_last_rx_ms[];      /* 0xFFFFFFFF = never received */",
        "void Head_Init(void);",
        "/* Decode one received frame; returns true if it matched a configured hub frame or custom RX frame. */",
        "bool Head_ProcessFrame(uint8_t bus, uint32_t can_id, bool extended, const uint8_t *data, uint8_t len, uint32_t now_ms);",
        "bool Head_ChannelIsStale(uint16_t slot, uint32_t now_ms);", "", "#endif /* HEAD_CONFIG_H */", ""]

    c = banner + ['#include "head_config.h"', ""]
    c += _arr("HeadChannel_t", "HEAD_CHANNELS", ch_rows) + _arr("HeadHubFrame_t", "HEAD_HUB_FRAMES", hub_frames)
    c += _arr("HeadHubRoute_t", "HEAD_HUB_ROUTES", routes) + _arr("HeadRxFrame_t", "HEAD_RX_FRAMES", rx_frames)
    c += _arr("HeadRxSignal_t", "HEAD_RX_SIGNALS", rx_signals) + filt_defs + _arr("HeadBus_t", "HEAD_BUSES", bus_rows)

    return {"head_config.h": "\n".join(h), "head_config_generated.c": "\n".join(c), "head_decode.c": DECODE_C}


DECODE_C = r'''/* head_decode.c - fixed runtime for the generated head tables (CAN-harness Master Configuration). */
#include "head_config.h"
#include <string.h>

#define NEVER 0xFFFFFFFFu
#define SLOTS (HEAD_CHANNEL_COUNT > 0 ? HEAD_CHANNEL_COUNT : 1)

volatile HeadValue_t g_head_values[SLOTS];
volatile uint32_t g_head_last_rx_ms[SLOTS];

void Head_Init(void)
{
    for (uint32_t i = 0; i < SLOTS; i++) {
        g_head_values[i].u = 0;
        g_head_last_rx_ms[i] = NEVER;
    }
}

bool Head_ChannelIsStale(uint16_t slot, uint32_t now_ms)
{
    if (slot >= HEAD_CHANNEL_COUNT || g_head_last_rx_ms[slot] == NEVER)
        return true;
    return (uint32_t)(now_ms - g_head_last_rx_ms[slot]) > HEAD_CHANNELS[slot].timeout_ms;
}

/* Intel: start_bit is the LSB. Motorola (DBC): start_bit is the MSB, sawtooth numbering. */
static uint32_t extract_bits(const uint8_t *d, uint8_t len, uint8_t start, uint8_t n, bool big_endian)
{
    uint32_t v = 0;
    uint16_t pos = start;
    for (uint8_t k = 0; k < n; k++) {
        uint8_t bit = (pos >> 3) < len ? (d[pos >> 3] >> (pos & 7)) & 1u : 0u;
        if (big_endian) {
            v = (v << 1) | bit;
            pos = (pos & 7) ? pos - 1 : pos + 15;
        } else {
            v |= (uint32_t)bit << k;
            pos++;
        }
    }
    return v;
}

static void process_hub(const uint8_t *data, uint8_t len, uint32_t can_id, uint32_t now_ms, bool *handled)
{
    for (uint16_t i = 0; i < HEAD_HUB_FRAME_COUNT; i++) {
        const HeadHubFrame_t *f = &HEAD_HUB_FRAMES[i];
        if ((can_id & f->id_mask) != f->id_base || len < f->min_len)
            continue;
        for (uint16_t r = 0; r < f->route_count; r++) {
            const HeadHubRoute_t *rt = &HEAD_HUB_ROUTES[f->first_route + r];
            uint32_t v = 0;
            for (uint8_t b = 0; b < rt->length; b++)
                v |= (uint32_t)data[rt->offset + b] << (8 * b);
            if (rt->is_signed && rt->length < 4 && (v & (1u << (8 * rt->length - 1))))
                v |= ~0u << (8 * rt->length);
            g_head_values[rt->slot].u = v;
            g_head_last_rx_ms[rt->slot] = now_ms;
        }
        *handled = true;
    }
}

static void process_rx(uint8_t bus, uint32_t can_id, bool extended, const uint8_t *data, uint8_t len, uint32_t now_ms, bool *handled)
{
    for (uint16_t i = 0; i < HEAD_RX_FRAME_COUNT; i++) {
        const HeadRxFrame_t *f = &HEAD_RX_FRAMES[i];
        if (f->bus != bus || (bool)f->extended != extended || (can_id & f->id_mask) != (f->can_id & f->id_mask))
            continue;
        int32_t mux = -1;
        if (f->mux_enabled)
            mux = (int32_t)extract_bits(data, len, f->mux_start_bit, f->mux_bit_length, false);
        for (uint16_t s = 0; s < f->signal_count; s++) {
            const HeadRxSignal_t *sg = &HEAD_RX_SIGNALS[f->first_signal + s];
            if (sg->mux_value >= 0 && sg->mux_value != mux)
                continue;
            uint32_t raw = extract_bits(data, len, sg->start_bit, sg->bit_length, sg->big_endian);
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
            g_head_values[sg->slot].f = v * sg->multiplier + sg->offset;
            g_head_last_rx_ms[sg->slot] = now_ms;
        }
        *handled = true;
    }
}

bool Head_ProcessFrame(uint8_t bus, uint32_t can_id, bool extended, const uint8_t *data, uint8_t len, uint32_t now_ms)
{
    bool handled = false;
    if (bus == HEAD_HUB_BUS_INDEX) {
        if (extended)
            process_hub(data, len, can_id, now_ms, &handled);
    } else {
        process_rx(bus, can_id, extended, data, len, now_ms, &handled);
    }
    return handled;
}
'''
