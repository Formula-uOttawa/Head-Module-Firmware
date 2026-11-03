"""Reference decoder for custom CAN receive frames (mirrors the generated C in head_decode.c)."""

from __future__ import annotations

import struct

from .model import RxFrame, RxSignal


def extract_bits(data: bytes, start_bit: int, length: int, order: str) -> int:
    """Raw unsigned bit field. Intel: start_bit is the LSB. Motorola (DBC): start_bit is the MSB (sawtooth numbering)."""
    if order == "intel":
        return (int.from_bytes(data, "little") >> start_bit) & ((1 << length) - 1)
    value, pos = 0, start_bit
    for _ in range(length):
        value = (value << 1) | ((data[pos // 8] >> (pos % 8)) & 1) if pos // 8 < len(data) else value << 1
        pos = pos - 1 if pos % 8 else pos + 15
    return value


def signal_value(data: bytes, s: RxSignal) -> float:
    raw = extract_bits(data, s.start_bit, s.bit_length, s.byte_order)
    if s.is_float:
        v = struct.unpack("<f", raw.to_bytes(4, "little"))[0]
    elif s.signed and raw & (1 << (s.bit_length - 1)):
        v = raw - (1 << s.bit_length)
    else:
        v = raw
    return v * s.multiplier + s.offset


def decode_frame(frame: RxFrame, data: bytes) -> dict[int, float]:
    """channel_id -> decoded value for every signal active for this payload (mux aware)."""
    mux = None
    if frame.mux_start_bit is not None:
        mux = extract_bits(data, frame.mux_start_bit, frame.mux_bit_length or 8, "intel")
    return {s.channel_id: signal_value(data, s) for s in frame.signals if s.mux_value is None or s.mux_value == mux}


def bit_span(s: RxSignal) -> set[int]:
    """Set of payload bit positions (Intel numbering, byte*8+bit) the signal occupies."""
    if s.byte_order == "intel":
        return set(range(s.start_bit, s.start_bit + s.bit_length))
    bits, pos = set(), s.start_bit
    for _ in range(s.bit_length):
        bits.add(pos)
        pos = pos - 1 if pos % 8 else pos + 15
    return bits
