"""Binary head config blob (format v1, see docs/protocol.md section 2)."""

from __future__ import annotations

import struct
import zlib

from .generators.head_c import head_tables
from .model import Project

MAGIC = 0x46434843
FORMAT_VERSION = 1
HEADER_LEN = 32
MAX_BLOB = 16384
LIMITS = {"channels": 256, "hub_frames": 64, "hub_routes": 512, "rx_frames": 64, "rx_signals": 256, "buses": 4, "filters": 128, "tx_frames": 32, "tx_signals": 128}

# id -> (name, struct format of one element, element size)
SECTIONS = {
    1: ("channels", "<HBBHHI", 12),
    2: ("hub_frames", "<IIBBHHH", 16),
    3: ("hub_routes", "<HBBBB2xff", 16),
    4: ("rx_frames", "<BBBBIIIHHBBH", 24),
    5: ("rx_signals", "<HBBBBBBhHff", 20),
    6: ("buses", "<BBBBII", 12),
    7: ("filters", "<BBHII", 12),
    8: ("tx_frames", "<BBBBIHHHH", 16),
    9: ("tx_signals", "<HBBBBBBff", 16),
}
for _id, (_n, _f, _sz) in SECTIONS.items():
    assert struct.calcsize(_f) == _sz, (_n, struct.calcsize(_f))


def build(project: Project) -> bytes:
    tables = head_tables(project)
    for name, limit in LIMITS.items():
        if len(tables[name]) > limit:
            raise ValueError(f"{name}: {len(tables[name])} exceeds firmware limit {limit}")
    body = bytearray()
    for sid, (name, fmt, size) in SECTIONS.items():
        rows = tables[name]
        body += struct.pack("<HHI", sid, size, len(rows))
        for r in rows:
            body += struct.pack(fmt, *r)
    total = HEADER_LEN + len(body)
    if total > MAX_BLOB:
        raise ValueError(f"config blob is {total} bytes, firmware limit is {MAX_BLOB}")
    head = struct.pack("<IHHIIQII", MAGIC, FORMAT_VERSION, HEADER_LEN, total, len(SECTIONS), int(project.config_hash(), 16), 0, 0)
    blob = bytearray(head + body)
    blob[28:32] = struct.pack("<I", zlib.crc32(bytes(blob)))
    return bytes(blob)


def parse(blob: bytes) -> dict:
    """Validate and decode a blob. Raises ValueError on any inconsistency."""
    if len(blob) < HEADER_LEN:
        raise ValueError("blob shorter than header")
    magic, ver, hlen, total, nsec, chash, _, crc = struct.unpack_from("<IHHIIQII", blob)
    if magic != MAGIC:
        raise ValueError("bad magic")
    if ver != FORMAT_VERSION:
        raise ValueError(f"unsupported format version {ver}")
    if total != len(blob) or hlen != HEADER_LEN:
        raise ValueError("length mismatch")
    z = bytearray(blob); z[28:32] = b"\0\0\0\0"
    if zlib.crc32(bytes(z)) != crc:
        raise ValueError("crc mismatch")
    out = {"config_hash": chash, "format_version": ver}
    off = HEADER_LEN
    for _ in range(nsec):
        sid, esize, count = struct.unpack_from("<HHI", blob, off)
        off += 8
        if off + esize * count > total:
            raise ValueError("section overruns blob")
        if sid in SECTIONS:
            name, fmt, size = SECTIONS[sid]
            if esize < size:
                raise ValueError(f"section {name}: element too small")
            out[name] = [struct.unpack_from(fmt, blob, off + i * esize) for i in range(count)]
        off += esize * count
    return out
