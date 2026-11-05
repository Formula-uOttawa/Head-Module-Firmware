"""Message encode/decode helpers (docs/protocol.md section 3)."""

from __future__ import annotations

import struct

INFO, CFG_BEGIN, CFG_CHUNK, CFG_COMMIT, CFG_READ, CFG_CLEAR = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
LIVE_GET, STATS_GET, SNIFF_SET, SNIFF_FRAME, REBOOT, ERROR = 0x10, 0x11, 0x30, 0xB1, 0x70, 0xFF
COMMIT_APPLY, COMMIT_STORE = 0x01, 0x02
CHUNK_MAX = 240

NAMES = {INFO: "INFO", CFG_BEGIN: "CFG_BEGIN", CFG_CHUNK: "CFG_CHUNK", CFG_COMMIT: "CFG_COMMIT", CFG_READ: "CFG_READ",
         CFG_CLEAR: "CFG_CLEAR", LIVE_GET: "LIVE_GET", STATS_GET: "STATS_GET", SNIFF_SET: "SNIFF_SET", REBOOT: "REBOOT"}


def name(mtype: int) -> str:
    return NAMES.get(mtype, f"0x{mtype:02X}")


ERRORS = {1: "bad request", 2: "busy / bad state", 3: "too big", 4: "crc mismatch", 5: "invalid config",
          6: "flash error", 7: "cm4 timeout", 8: "unsupported", 9: "out of range"}


class DeviceError(Exception):
    def __init__(self, request_type: int, code: int):
        super().__init__(f"{name(request_type)} failed: {ERRORS.get(code, code)}")
        self.request_type, self.code = request_type, code


def parse_info(p: bytes) -> dict:
    (proto, blob, fw, up, ah, sh, src, cm4, maxch, maxblob, chans) = struct.unpack_from("<BBHIQQBBHHH", p)
    return {"proto_version": proto, "blob_version": blob, "fw_version": f"{fw >> 8}.{fw & 0xFF}", "uptime_ms": up,
            "active_hash": f"{ah:016x}", "stored_hash": f"{sh:016x}" if sh else None,
            "active_source": ["default", "flash", "uploaded"][src] if src < 3 else src, "cm4_alive": bool(cm4),
            "max_channels": maxch, "max_blob": maxblob, "channel_count": chans, "uid": p[32:44].hex(),
            "fdcan_kernel_hz": struct.unpack_from("<I", p, 44)[0] if len(p) >= 48 else None}


def parse_live(p: bytes) -> dict:
    first, count = struct.unpack_from("<HH", p)
    vals = [struct.unpack_from("<IB", p, 4 + 5 * i) for i in range(count)]
    return {"first": first, "raw": [v for v, _ in vals], "stale": [bool(f & 1) for _, f in vals]}


def parse_stats(p: bytes) -> dict:
    nbus = p[0]
    buses, off = [], 1
    for _ in range(nbus):
        rx, matched, dropped, lost, off_, pas = struct.unpack_from("<IIIIBB", p, off)
        off += 18
        buses.append({"rx_frames": rx, "rx_matched": matched, "rx_dropped": dropped, "fifo_lost": lost,
                      "bus_off": bool(off_), "error_passive": bool(pas)})
    nh = p[off]
    off += 1 + 4 * nh
    hub_lost = list(struct.unpack_from(f"<{nh}I", p, off - 4 * nh))
    if off < len(p) and p[off] == nbus and off + 1 + 8 * nbus <= len(p):  # transmit counters (newer firmware)
        for i, b in enumerate(buses):
            b["tx_frames"], b["tx_dropped"] = struct.unpack_from("<II", p, off + 1 + 8 * i)
    return {"buses": buses, "hub_lost": hub_lost}


def parse_sniff(p: bytes) -> dict:
    bus, flags, cid, ts, ln = struct.unpack_from("<BBIIB", p)
    return {"bus": bus, "extended": bool(flags & 1), "fd": bool(flags & 2), "id": cid, "ts_ms": ts, "data": p[11:11 + ln]}
