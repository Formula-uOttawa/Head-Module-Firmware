"""Import SensorHubV1/config/rear_sensor_config.yaml into a Project."""

from __future__ import annotations

import re
from pathlib import Path

import yaml

from .model import Channel, Hub, Project, SensorType


def _short(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_")
    return s if s and not s[0].isdigit() else f"_{s}"


def import_yaml(path: Path, hub_name: str = "Hub") -> Project:
    return from_data(yaml.safe_load(path.read_text(encoding="utf-8")), path.stem, hub_name)


def from_data(data: dict, name: str = "legacy", hub_name: str = "Hub") -> Project:
    project = Project(name=name)
    hubs: set[int] = set()
    for i, c in enumerate(data["channels"], start=1):
        ct = c["can_target"]
        hub = int(ct["hub_id"])
        hubs.add(hub)
        ref = c.get("ref_channel", "GND")
        project.channels.append(Channel(
            channel_id=i,
            name=c["name"],
            short_name=_short(c["name"]),
            hub=hub,
            sensor_type=SensorType(str(c["sensor_type"]).lower()),
            sample_rate_hz=int(c["sample_rate_hz"]),
            pin_sig_pos=int(c["channel"]),
            pin_sig_neg=None if ref == "GND" else int(ref),
            raw_bytes=int(ct["byte_length"]),
            frame_idx=int(ct.get("frame_idx", ct.get("can_frame", 0))),
            byte_offset=int(ct["byte_offset"]),
        ))
    project.hubs = [Hub(hub_id=h, name=f"{hub_name} {h}") for h in sorted(hubs)]
    return project
