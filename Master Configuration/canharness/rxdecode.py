"""Reference decoder for custom CAN receive frames (mirrors the generated C in head_decode.c)."""

from __future__ import annotations

import struct

from .model import RxFrame, RxSignal, TxSignal


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


def bit_span(s: RxSignal | TxSignal) -> set[int]:
    """Set of payload bit positions (Intel numbering, byte*8+bit) the signal occupies."""
    if s.byte_order == "intel":
        return set(range(s.start_bit, s.start_bit + s.bit_length))
    bits, pos = set(), s.start_bit
    for _ in range(s.bit_length):
        bits.add(pos)
        pos = pos - 1 if pos % 8 else pos + 15
    return bits


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def insert_bits(buf: bytearray, start_bit: int, length: int, order: str, raw: int) -> None:
    """Inverse of extract_bits: write the low `length` bits of raw into buf."""
    if order == "intel":
        positions = range(start_bit, start_bit + length)
        bits = [(raw >> k) & 1 for k in range(length)]
    else:
        positions, pos = [], start_bit
        for _ in range(length):
            positions.append(pos)
            pos = pos - 1 if pos % 8 else pos + 15
        bits = [(raw >> (length - 1 - k)) & 1 for k in range(length)]
    for pos, bit in zip(positions, bits):
        if pos // 8 < len(buf):
            buf[pos // 8] = (buf[pos // 8] & ~(1 << (pos % 8))) | (bit << (pos % 8))


def encode_tx_signal(buf: bytearray, s: TxSignal, value: float) -> None:
    """Reference for the firmware's transmit encoder: raw = round((value - offset) / multiplier), saturated to the field."""
    scaled = f32(f32(value) - f32(s.offset)) / f32(s.multiplier)  # firmware computes in float32
    scaled = f32(scaled)
    if s.is_float:
        raw = int.from_bytes(struct.pack("<f", scaled), "little")
    else:
        lo, hi = (-(1 << (s.bit_length - 1)), (1 << (s.bit_length - 1)) - 1) if s.signed else (0, (1 << s.bit_length) - 1)
        raw = int(max(lo, min(hi, int(scaled + 0.5) if scaled >= 0 else -int(-scaled + 0.5)))) & ((1 << s.bit_length) - 1)
    insert_bits(buf, s.start_bit, s.bit_length, s.byte_order, raw)


def encode_tx_frame(frame, values: dict[int, float]) -> bytes:
    buf = bytearray(frame.dlc)
    for s in frame.signals:
        encode_tx_signal(buf, s, values.get(s.channel_id, 0.0))
    return bytes(buf)
