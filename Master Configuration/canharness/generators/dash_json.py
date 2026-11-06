"""Generate the dash channels.json (decode info + calibration, everything the dash needs)."""

from __future__ import annotations

import json

from .. import hwmap
from ..model import Project
from ..packing import pack


def generate_dash(project: Project) -> str:
    frames, _ = pack(project)
    where = {p.channel.channel_id: (fr, p) for fr in frames for p in fr.placements}
    chans = []
    for ch in project.channels:
        if not ch.enabled:
            continue
        analog = ch.source == "hub" and ch.sensor_type.value != "digital"
        entry = {
            "channel_id": ch.channel_id,
            "name": ch.name,
            "short_name": ch.short_name,
            "source": ch.source,
            "sensor_type": ch.sensor_type.value,
            "sample_rate_hz": ch.sample_rate_hz,
            "units": ch.units,
            "scaling": ch.scaling.model_dump(),
            "fault_low": ch.fault_low,
            "fault_high": ch.fault_high,
            "calibration": {"x": ch.cal_x, "y": ch.cal_y} if ch.cal_x else None,
            # what the head distributes: CAN-receive channels are scaled floats, analog hub channels are float32 VOLTS at the sensor pin
            # (resistive inputs: OHMS = volts / excitation current); calibration x values are in that base unit; digital hub channels are integers
            "value_type": "f32" if analog or ch.source == "can_rx" else ("i32" if ch.signed else "u32"),
            **({"base_unit": hwmap.base_unit(ch.sensor_type.value),
                "volts_per_count": hwmap.volts_per_count(ch.sensor_type.value, project.settings.afe_vref_v),
                "excitation_current_a": hwmap.excitation_current_a(ch.sensor_type.value) or None} if analog else {}),
            "raw": {"bytes": ch.raw_bytes, "signed": ch.signed, "endian": "little",
                    # analog hub channels without a fresh AFE sample are sent as the minimum 24-bit code ("no data")
                    **({"no_data_value": -8388608} if ch.source == "hub" and ch.sensor_type.value != "digital" else {})},
        }
        if ch.channel_id in where:
            fr, p = where[ch.channel_id]
            entry["frame"] = {
                "hub": fr.hub,
                "id_base": fr.id_base,
                "frame_idx": fr.frame_idx,
                "byte_offset": p.offset,
            }
        chans.append(entry)
    doc = {
        "schema_version": project.schema_version,
        "config_hash": project.config_hash(),
        "project": project.name,
        "udp": {"ip": project.settings.udp_ip, "port": project.settings.udp_port},
        "channels": chans,
    }
    return json.dumps(doc, indent=2) + "\n"
