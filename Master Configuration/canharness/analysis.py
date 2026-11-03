"""Derived views for the UI: packed frames, CAN-FD bus load estimate, hardware info."""

from __future__ import annotations

from . import canid, hwmap
from .model import Project
from .packing import pack
from .validate import validate


def frame_bits(payload_bytes: int) -> tuple[int, float]:
    """(nominal-phase bits, data-phase bits) of one FD+BRS extended-ID frame (estimate, +10% data stuffing)."""
    nominal = 35 + 13  # SOF..BRS and CRC-delim/ACK/EOF/IFS
    crc = 21 if payload_bytes > 16 else 17
    data = (1 + 4 + 8 * payload_bytes + 4 + crc) * 1.1
    return nominal, data


def analyze(project: Project) -> dict:
    frames, pack_errors = pack(project)
    hub = next((b for b in project.buses if b.role == "hub"), None)
    nominal_bps = hub.bitrate if hub else 320_000
    data_bps = (hub.data_bitrate or nominal_bps) if hub else 3_200_000
    load = 0.0
    out_frames = []
    for f in frames:
        nom, dat = frame_bits(f.dlc_bytes)
        f_load = f.rate_hz * (nom / nominal_bps + dat / data_bps)
        load += f_load
        out_frames.append({
            "hub": f.hub, "rate_hz": f.rate_hz, "frame_idx": f.frame_idx,
            "id_base": f.id_base, "payload_bytes": f.payload_bytes, "dlc_bytes": f.dlc_bytes,
            "load": f_load,
            "placements": [{"channel_id": p.channel.channel_id, "short_name": p.channel.short_name,
                            "offset": p.offset, "length": p.length} for p in f.placements],
        })
    ext_loads = {}
    for b in project.buses:
        if b.role != "external":
            continue
        bits = 0.0
        for rf in project.rx_frames:
            if rf.bus == b.id and rf.rate_hz:
                bits += rf.rate_hz * ((29 if rf.extended else 11) + 36 + 8 * rf.dlc) * 1.1  # classic CAN estimate
        ext_loads[b.id] = bits / b.bitrate
    return {
        "bus_nominal_bps": nominal_bps, "bus_data_bps": data_bps, "ext_loads": ext_loads,
        "issues": [i.__dict__ for i in validate(project)],
        "frames": out_frames,
        "bus_load": load,
        "config_hash": project.config_hash(),
    }


def hw_info() -> dict:
    pins = []
    for pin in range(1, 35):
        role, afe, idx = "none", None, None
        if pin in hwmap.AFE1_PINS:
            role, afe, idx = "analog", "A", hwmap.AFE1_PINS[pin]
        elif pin in hwmap.AFE2_PINS:
            role, afe, idx = "analog", "B", hwmap.AFE2_PINS[pin]
        elif pin in hwmap.DIGITAL_PINS:
            role, idx = "digital", hwmap.DIGITAL_PINS[pin]
        elif pin in hwmap.CONN1_GND_PINS:
            role = "gnd"
        if pin in hwmap.TC_NEG_PINS:
            role = "tc_neg" if pin != hwmap.TC_REF_PIN else "analog_tc_neg"
        pins.append({"pin": pin, "role": role, "afe": afe, "index": idx})
    return {
        "conn1": pins,
        "conn2": [{"pin": p, "role": "5v" if p in hwmap.CONN2_5V_PINS else "gnd"} for p in range(1, 35)],
        "tc_neg_pins": list(hwmap.TC_NEG_PINS),
        "rate_codes": sorted(canid.RATE_CODES),
        "afe_sps": hwmap.AFE_SPS,
    }
