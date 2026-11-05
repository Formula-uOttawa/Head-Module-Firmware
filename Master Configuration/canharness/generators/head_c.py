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


def head_tables(project: Project) -> dict[str, list[tuple]]:
    """Row tuples for every blob section, in the field order of docs/protocol.md section 2."""
    chans = channel_table(project)
    slot = {c.channel_id: i for i, c in enumerate(chans)}
    frames, _ = pack(project)
    bus_index = {b.id: i for i, b in enumerate(project.buses)}
    plans = plan_filters(project)

    hub_frames, routes = [], []
    for f in frames:
        hub_frames.append((f.id_base, HUB_FRAME_MASK, f.hub, f.payload_bytes, len(routes), len(f.placements), 0))
        for p in f.placements:
            routes.append((slot[p.channel.channel_id], p.offset, p.length, int(p.channel.signed)))

    rx_frames, rx_signals = [], []
    for rf in project.rx_frames:
        sigs = [s for s in rf.signals if s.channel_id in slot]
        mask = rf.id_mask if rf.id_mask is not None else (EXT_MASK if rf.extended else STD_MASK)
        rx_frames.append((bus_index[rf.bus], int(rf.extended), rf.dlc, int(rf.mux_start_bit is not None), rf.can_id, mask,
                          rf.timeout_ms, len(rx_signals), len(sigs), rf.mux_start_bit or 0, rf.mux_bit_length or 0, 0))
        for s in sigs:
            rx_signals.append((slot[s.channel_id], s.start_bit, s.bit_length, int(s.byte_order == "motorola"), int(s.signed),
                               int(s.is_float), 0, -1 if s.mux_value is None else s.mux_value, 0, s.multiplier, s.offset))

    tx_frames, tx_signals = [], []
    for tf in project.tx_frames:
        sigs = [s for s in tf.signals if s.channel_id in slot]
        tx_frames.append((bus_index[tf.bus], int(tf.extended), tf.dlc, 0, tf.can_id, max(1, round(1000 / tf.rate_hz)),
                          len(tx_signals), len(sigs), 0))
        for s in sigs:
            tx_signals.append((slot[s.channel_id], s.start_bit, s.bit_length, int(s.byte_order == "motorola"), int(s.signed),
                               int(s.is_float), 0, s.multiplier, s.offset))

    channels = []
    for c in chans:
        if c.source == "can_rx":
            frame = next((rf for rf in project.rx_frames if any(s.channel_id == c.channel_id for s in rf.signals)), None)
            channels.append((c.channel_id, 2, 1, c.sample_rate_hz, 0, frame.timeout_ms if frame else 1000))
        else:
            channels.append((c.channel_id, 1 if c.signed else 0, 0, c.sample_rate_hz, 0, timeout_for_rate(c.sample_rate_hz)))

    buses, filters = [], []
    for i, b in enumerate(project.buses):
        per = int(b.head_peripheral[-1]) if b.head_peripheral.upper().startswith("FDCAN") and b.head_peripheral[-1].isdigit() else 0
        buses.append((int(b.role == "hub"), int(b.type == "can_fd"), per, 0, b.bitrate, b.data_bitrate or 0))
        for kind, ext in (("std", 0), ("ext", 1)):
            filters += [(i, ext, 0, fid, fmask) for fid, fmask in plans[b.id][kind]]
    return {"channels": channels, "hub_frames": hub_frames, "hub_routes": routes, "rx_frames": rx_frames,
            "rx_signals": rx_signals, "buses": buses, "filters": filters, "tx_frames": tx_frames, "tx_signals": tx_signals}


def generate_head(project: Project) -> dict[str, bytes | str]:
    """Head artefacts: the binary blob (uploadable) and a C file embedding it as the compiled-in default config."""
    from .. import blob as blobmod  # late import: blob depends on head_tables

    data = blobmod.build(project)
    rows = [", ".join(f"0x{b:02X}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    c = [
        "/*", " * AUTO GENERATED by CAN-harness Master Configuration - DO NOT EDIT",
        f" * project '{project.name}'  config hash {project.config_hash()}", " * Compiled-in default head configuration (blob format v1).", " */",
        "#include <stdint.h>", "",
        "const uint8_t HEAD_DEFAULT_BLOB[] __attribute__((aligned(4))) = {", *[f"    {r}," for r in rows], "};",
        f"const uint32_t HEAD_DEFAULT_BLOB_LEN = {len(data)};", "",
    ]
    return {"head_default.blob": data, "head_default_blob.c": "\n".join(c)}
