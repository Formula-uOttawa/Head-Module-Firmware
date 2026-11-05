"""Pack hub channels into CAN data frames per (hub, rate)."""

from __future__ import annotations

from dataclasses import dataclass, field

from . import canid
from .model import Channel, Project, SensorType


FD_LENGTHS = [8, 12, 16, 20, 24, 32, 48, 64]


@dataclass
class Placement:
    channel: Channel
    offset: int
    length: int


@dataclass
class PackedFrame:
    hub: int
    rate_hz: int
    frame_idx: int
    placements: list[Placement] = field(default_factory=list)

    @property
    def payload_bytes(self) -> int:
        return max((p.offset + p.length for p in self.placements), default=4)

    @property
    def dlc_bytes(self) -> int:
        """Payload rounded up to a legal CAN-FD length (the hub firmware does the same)."""
        return next(n for n in FD_LENGTHS if n >= self.payload_bytes)

    @property
    def id_base(self) -> int:
        return canid.id_base(canid.MSG_DATA, self.hub, self.rate_hz, self.frame_idx)


def hub_channels(project: Project) -> list[Channel]:
    return [c for c in project.channels if c.enabled and c.source == "hub" and c.hub is not None]


def pack(project: Project) -> tuple[list[PackedFrame], list[str]]:
    """Return (frames, errors). Explicit placements are honoured first, the rest are first-fit."""
    s = project.settings
    errors: list[str] = []
    frames: dict[tuple[int, int, int], PackedFrame] = {}
    used: dict[tuple[int, int, int], list[tuple[int, int, str]]] = {}

    def frame(hub: int, rate: int, idx: int) -> PackedFrame:
        key = (hub, rate, idx)
        if key not in frames:
            frames[key] = PackedFrame(hub, rate, idx)
            used[key] = []
        return frames[key]

    def fits(key, off, ln) -> bool:
        return s.data_start <= off and off + ln <= s.data_end and all(
            off + ln <= o or o + l <= off for o, l, _ in used[key]
        )

    def put(ch: Channel, idx: int, off: int) -> None:
        f = frame(ch.hub, ch.sample_rate_hz, idx)
        f.placements.append(Placement(ch, off, ch.raw_bytes))
        used[(ch.hub, ch.sample_rate_hz, idx)].append((off, ch.raw_bytes, ch.short_name))

    chans = sorted(hub_channels(project), key=lambda c: c.channel_id)
    auto: list[Channel] = []
    for ch in chans:
        if ch.sensor_type == SensorType.CAN:
            continue
        if ch.frame_idx is None or ch.byte_offset is None:
            auto.append(ch)
            continue
        key = (ch.hub, ch.sample_rate_hz, ch.frame_idx)
        frame(*key)
        if not fits(key, ch.byte_offset, ch.raw_bytes):
            errors.append(
                f"[{ch.short_name}]: bytes {ch.byte_offset}-{ch.byte_offset + ch.raw_bytes - 1} in frame "
                f"{ch.frame_idx} are outside data area {s.data_start}-{s.data_end - 1} or overlap another signal"
            )
            continue
        put(ch, ch.frame_idx, ch.byte_offset)

    auto.sort(key=lambda c: -c.raw_bytes)  # largest first keeps natural alignment gap-free
    for ch in auto:
        placed = False
        for idx in range(32):
            key = (ch.hub, ch.sample_rate_hz, idx)
            frame(*key)
            step = {1: 1, 2: 2, 3: 1, 4: 4}.get(ch.raw_bytes, 1)  # natural alignment; 3-byte fields need none
            off = -(-s.data_start // step) * step  # natural alignment
            while off + ch.raw_bytes <= s.data_end:
                if fits(key, off, ch.raw_bytes):
                    put(ch, idx, off)
                    placed = True
                    break
                off += step
            if placed:
                break
        if not placed:
            errors.append(f"[{ch.short_name}]: no room left in 32 frames for hub {ch.hub} @ {ch.sample_rate_hz} Hz")

    out = [f for f in frames.values() if f.placements]
    out.sort(key=lambda f: (f.hub, -f.rate_hz, f.frame_idx))
    for f in out:
        f.placements.sort(key=lambda p: p.offset)
    return out, errors
