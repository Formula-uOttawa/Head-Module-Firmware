"""Binary hub config blob (format v1, docs/protocol.md section 2.1). Same container as the head blob with its own magic."""

from __future__ import annotations

import struct
import zlib

from .generators.hub_c import MAX_SENSORS, hub_sensor_rows
from .model import Project

MAGIC = 0x42554843  # 'C' 'H' 'U' 'B'
FORMAT_VERSION = 1
HEADER_LEN = 32
MAX_BLOB = 1024
SEC_META = 0x21
SEC_SENSORS = 0x22
SENSOR_FMT = "<I8B7BB"  # frame_id, group channel type rate offset length slot rsvd, 7 AFE registers, rsvd
SENSOR_SIZE = 20
assert struct.calcsize(SENSOR_FMT) == SENSOR_SIZE


def build(project: Project, hub_id: int) -> bytes:
    if not 0 <= hub_id <= 7:
        raise ValueError("hub id must be 0..7")
    rows = hub_sensor_rows(project, hub_id)
    if len(rows) > MAX_SENSORS:
        raise ValueError(f"hub {hub_id}: {len(rows)} sensors exceed the firmware limit {MAX_SENSORS}")
    body = struct.pack("<HHI", SEC_META, 8, 1) + struct.pack("<B7x", hub_id)
    body += struct.pack("<HHI", SEC_SENSORS, SENSOR_SIZE, len(rows))
    for fid, group, chan, stype, rate, off, ln, slot, *regs in rows:
        body += struct.pack(SENSOR_FMT, fid, group, chan, stype, rate, off, ln, slot, 0, *regs, 0)
    total = HEADER_LEN + len(body)
    if total > MAX_BLOB:
        raise ValueError(f"hub config blob is {total} bytes, firmware limit is {MAX_BLOB}")
    head = struct.pack("<IHHIIQII", MAGIC, FORMAT_VERSION, HEADER_LEN, total, 2, int(project.config_hash(), 16), 0, 0)
    blob = bytearray(head + body)
    blob[28:32] = struct.pack("<I", zlib.crc32(bytes(blob)))
    return bytes(blob)


def parse(blob: bytes) -> dict:
    """Validate and decode a hub blob. Raises ValueError on any inconsistency."""
    if len(blob) < HEADER_LEN:
        raise ValueError("blob shorter than header")
    magic, ver, hlen, total, nsec, chash, _, crc = struct.unpack_from("<IHHIIQII", blob)
    if magic != MAGIC:
        raise ValueError("bad magic (not a hub config)")
    if ver != FORMAT_VERSION:
        raise ValueError(f"unsupported format version {ver}")
    if total != len(blob) or hlen != HEADER_LEN:
        raise ValueError("length mismatch")
    z = bytearray(blob); z[28:32] = b"\0\0\0\0"
    if zlib.crc32(bytes(z)) != crc:
        raise ValueError("crc mismatch")
    out = {"config_hash": chash, "format_version": ver, "hub_id": None, "sensors": []}
    off = HEADER_LEN
    for _ in range(nsec):
        sid, esize, count = struct.unpack_from("<HHI", blob, off)
        off += 8
        if off + esize * count > total:
            raise ValueError("section overruns blob")
        if sid == SEC_META and esize >= 8 and count == 1:
            out["hub_id"] = blob[off]
        elif sid == SEC_SENSORS and esize >= SENSOR_SIZE:
            out["sensors"] = [struct.unpack_from(SENSOR_FMT, blob, off + i * esize) for i in range(count)]
        off += esize * count
    return out
