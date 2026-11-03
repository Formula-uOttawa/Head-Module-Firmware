"""Generate SensorHubV1 sensor_config_generated.c (same extern contract as sensor_state.h)."""

from __future__ import annotations

from .. import hwmap
from ..model import Project
from ..packing import PackedFrame, pack

MAX_SENSORS = 24


def _afe_entry(ch, afe: int, afe_pin: int) -> str:
    st = ch.sensor_type.value
    neg = hwmap.AINCOM if ch.pin_sig_neg is None else hwmap.resolve_pin(ch.pin_sig_neg)[1]
    f = [
        ("inpmux", hwmap.inpmux(st, afe_pin, neg)),
        ("pga", hwmap.pga(st)),
        ("datarate", hwmap.datarate(afe, st)),
        ("ref", hwmap.ref_ctrl(st)),
        ("idacmag", hwmap.idacmag(st)),
        ("idacmux", hwmap.idacmux(st, afe_pin)),
        ("vbias", hwmap.vbias(st, afe_pin)),
    ]
    body = ",\n".join(f"        .{k:<8} = 0x{v:02X}" for k, v in f)
    return f"    // {ch.name}\n    {{\n{body}\n    }},"


def generate_hub(project: Project, hub_id: int) -> str:
    frames, _ = pack(project)
    where = {}  # channel_id -> (frame, placement)
    for fr in frames:
        for p in fr.placements:
            where[p.channel.channel_id] = (fr, p)

    groups = {1: [], 2: [], 3: []}
    for ch in project.channels:
        if ch.enabled and ch.source == "hub" and ch.hub == hub_id and ch.channel_id in where:
            afe, afe_pin = hwmap.resolve_pin(ch.pin_sig_pos)
            groups[afe].append((ch, afe, afe_pin))

    out = [
        "#include <stdint.h>",
        '#include "sensor_state.h"',
        "",
        "/*",
        "--------------------------------",
        "AUTO GENERATED SENSOR CONFIGURATION FILE (CAN-harness Master Configuration)",
        f"hub {hub_id}  config hash {project.config_hash()}",
        "DO NOT EDIT - regenerate from the project file",
        "--------------------------------",
        "*/",
        f"volatile uint32_t LIVE_SENSOR_DATA[MAX_SENSORS];  // one slot per connector pin (pin - 1)",
        "",
    ]
    for afe, prefix in ((1, "A"), (2, "B"), (3, "DIG")):
        items = groups[afe]
        out.append(f"const SensorConfig_t {prefix}_ACTIVE_SENSORS_CONFIG[] = {{")
        for ch, _, afe_pin in items:
            fr, p = where[ch.channel_id]
            out += [
                f"    // {ch.name}", "    {",
                f"        .frame_id    = 0x{fr.id_base:08X}UL,",
                f"        .afe         = {afe},",
                f"        .afe_channel = {afe_pin},",
                f"        .sensor_type = SENSOR_TYPE_{ch.sensor_type.name},",
                f"        .sample_rate = RATE_{ch.sample_rate_hz}HZ,",
                f"        .byte_offset = {p.offset},",
                f"        .byte_length = {p.length},",
                f"        .live_sensor_mapping = {ch.pin_sig_pos - 1}",
                "    },",
            ]
        out += ["};", f"const uint8_t {prefix}_ACTIVE_SENSORS_COUNT = {len(items)};", ""]
        if afe != 3:
            out.append(f"const SensorAfeConfig_t {prefix}_ACTIVE_SENSORS_AFE_CONFIG[] = {{")
            out += [_afe_entry(ch, a, pin) for ch, a, pin in items]
            out += ["};", ""]
    return "\n".join(out)
