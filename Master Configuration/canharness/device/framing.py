"""COBS + CRC-16/CCITT-FALSE framing for the UART transport."""

from __future__ import annotations


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out, block, code = bytearray(), bytearray(), 1
    for b in data:
        if b == 0:
            out.append(code); out += block; block.clear(); code = 1
        else:
            block.append(b); code += 1
            if code == 0xFF:
                out.append(code); out += block; block.clear(); code = 1
    out.append(code); out += block
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data):
            raise ValueError("bad COBS block")
        out += data[i + 1:i + code]
        i += code
        if code < 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def encode_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    body = bytes([msg_type, seq]) + payload
    body += crc16(body).to_bytes(2, "little")
    return b"\x00" + cobs_encode(body) + b"\x00"


def decode_body(body: bytes) -> tuple[int, int, bytes]:
    if len(body) < 4:
        raise ValueError("frame too short")
    if crc16(body[:-2]) != int.from_bytes(body[-2:], "little"):
        raise ValueError("crc mismatch")
    return body[0], body[1], bytes(body[2:-2])


class FrameParser:
    """Splits a byte stream into binary frames and ASCII lines (mirrors the firmware state machine)."""

    def __init__(self):
        self.mode = "idle"
        self.buf = bytearray()
        self.bad_frames = 0  # frames dropped for a bad CRC / COBS error

    def feed(self, data: bytes) -> list:
        """Return a list of ('frame', (type, seq, payload)) or ('text', str) items. State is always fully updated."""
        out = []
        for b in data:
            if self.mode == "binary":
                if b == 0:
                    if self.buf:
                        try:
                            out.append(("frame", decode_body(cobs_decode(bytes(self.buf)))))
                        except ValueError:
                            self.bad_frames += 1
                    self.buf.clear()
                    self.mode = "idle"
                else:
                    self.buf.append(b)
                    if len(self.buf) > 300:
                        self.buf.clear(); self.mode = "idle"
            elif self.mode == "text":
                if b in (0x0A, 0x0D):
                    out.append(("text", self.buf.decode("ascii", "replace").strip()))
                    self.buf.clear(); self.mode = "idle"
                elif b == 0:
                    self.buf.clear(); self.mode = "binary"
                else:
                    self.buf.append(b)
            else:  # idle: first byte decides
                if b == 0:
                    self.mode = "binary"; self.buf.clear()
                elif b not in (0x0A, 0x0D):
                    self.mode = "text"; self.buf = bytearray([b])
        return out
