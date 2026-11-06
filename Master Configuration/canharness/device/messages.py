"""Message encode/decode helpers (docs/protocol.md section 3)."""

from __future__ import annotations

import struct

INFO, CFG_BEGIN, CFG_CHUNK, CFG_COMMIT, CFG_READ, CFG_CLEAR, IDENT = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
DEVICE_HEAD, DEVICE_HUB = 1, 2
LIVE_GET, STATS_GET, SNIFF_SET, SNIFF_FRAME, HUB_RELAY, REBOOT, ERROR = 0x10, 0x11, 0x30, 0xB1, 0x40, 0x70, 0xFF
HUB_ASCII = 0x20  # hub only: request = a console command line, reply = the text it printed (the UART console over any transport)
COMMIT_APPLY, COMMIT_STORE = 0x01, 0x02
REBOOT_MAGIC = 0xB007F00D  # REBOOT must carry this little-endian u32, so a stray frame cannot reset the board
CHUNK_MAX = 240

NAMES = {INFO: "INFO", IDENT: "IDENT", CFG_BEGIN: "CFG_BEGIN", CFG_CHUNK: "CFG_CHUNK", CFG_COMMIT: "CFG_COMMIT", CFG_READ: "CFG_READ",
         CFG_CLEAR: "CFG_CLEAR", LIVE_GET: "LIVE_GET", STATS_GET: "STATS_GET", SNIFF_SET: "SNIFF_SET", REBOOT: "REBOOT",
         HUB_RELAY: "HUB_RELAY", HUB_ASCII: "HUB_ASCII"}


def name(mtype: int) -> str:
    return NAMES.get(mtype, f"0x{mtype:02X}")


ERRORS = {1: "bad request", 2: "busy / bad state", 3: "too big", 4: "crc mismatch", 5: "invalid config",
          6: "flash error", 7: "cm4 timeout", 8: "unsupported", 9: "out of range"}


class DeviceError(Exception):
    def __init__(self, request_type: int, code: int):
        super().__init__(f"{name(request_type)} failed: {ERRORS.get(code, code)}")
        self.request_type, self.code = request_type, code


class HubTimeout(DeviceError):
    """A hub reached through the head did not answer on the CAN bus (not connected, wrong hub id, hub not powered / CAN not running)."""

    def __init__(self, hub: int, request_type: int):
        Exception.__init__(self, f"hub {hub} did not answer {name(request_type)} over CAN (check power, wiring, hub id and that its CAN started)")
        self.request_type, self.code, self.hub = request_type, 7, hub


class WrongDevice(DeviceError):
    """The connected board does not implement the requested operation (e.g. config upload on a hub)."""

    def __init__(self, what: str, device: str):
        Exception.__init__(self, f"{what} is not supported by a {device} module")
        self.request_type, self.code = 0, 8


def parse_ident(p: bytes) -> dict:
    dev, proto, fw, inst = struct.unpack_from("<BBHB", p)
    return {"device_type": "hub" if dev == DEVICE_HUB else "head" if dev == DEVICE_HEAD else f"unknown({dev})",
            "proto_version": proto, "fw_version": f"{fw >> 8}.{fw & 0xFF}", "hub_id": None if inst == 0xFF else inst,
            "uid": p[6:18].hex()}


HUB_FLAGS = {0: "can_running", 1: "acq_running", 2: "afe_cfg_bad", 3: "tcan_ok", 4: "vcan_ok", 5: "sched_cfg_bad", 6: "afe_fault"}
TCAN_INIT_BITS = {0x01: "watchdog disable needed a retry", 0x02: "watchdog could not be disabled", 0x08: "could not enter normal mode",
                  0x10: "standby watchdog not disabled", 0x20: "warm start (SBC already configured)", 0x40: "VCAN not in regulation",
                  0x80: "CAN transceiver not enabled"}
HEAD_TCAN_INIT_BITS = {0x01: "watchdog disable needed a retry", 0x02: "watchdog could not be disabled", 0x04: "SBC init failed",
                       0x08: "could not enter normal mode", 0x10: "standby watchdog not disabled", 0x20: "warm start",
                       0x40: "VCAN not in regulation", 0x80: "CAN transceiver not enabled"}
AFE_STATUS_BITS = {0x01: "AFE A responds", 0x02: "AFE B responds", 0x04: "AFE A running", 0x08: "AFE B running",
                   0x10: "AFE A config ok", 0x20: "AFE B config ok", 0x80: "SPI2 not in mode 1"}


CFG_STATUS = {1: "too short", 2: "bad magic", 3: "unsupported version", 4: "bad length", 5: "crc mismatch", 6: "bad section",
              7: "over limit", 8: "value out of range"}


def parse_hub_info(p: bytes) -> dict:
    proto, _, fw, up, hub_id, flags, na, nb, nd, tcan, tec, rec, bus_off, passive = struct.unpack_from("<BBHIBBBBBBBBBB", p)
    sysclk, kernel, ok, fail, skipped, invalid = struct.unpack_from("<IIIIII", p, 20)
    info = {"device_type": "hub", "proto_version": proto, "fw_version": f"{fw >> 8}.{fw & 0xFF}", "uptime_ms": up,
            "hub_id": None if hub_id == 0xFF else hub_id, "can_running": bool(flags & 1),
            "flags": {name: bool(flags & (1 << bit)) for bit, name in HUB_FLAGS.items()},
            "sensors": {"afe_a": na, "afe_b": nb, "digital": nd}, "tcan_int": tcan,
            "can": {"tec": tec, "rec": rec, "bus_off": bool(bus_off), "error_passive": bool(passive)},
            "sysclk_hz": sysclk, "fdcan_kernel_hz": kernel,
            "tx": {"ok": ok, "failed": fail, "skipped": skipped, "invalid": invalid}, "uid": p[44:56].hex()}
    if len(p) >= 88:  # fields appended by newer hub firmware
        full, spi_err, resyncs, overrun, rx_over, uart_err, tx_fail = struct.unpack_from("<IIIIIII", p, 56)
        init, afe, inputs = p[84], p[85], p[86]
        info["tx"]["drop_full"] = full
        info["afe"] = {"spi_errors": spi_err, "resyncs": resyncs, "drdy_overruns": overrun, "status": afe,
                       "status_text": [t for bit, t in AFE_STATUS_BITS.items() if afe & bit]}
        info["uart"] = {"rx_overflow": rx_over, "errors": uart_err, "tx_failures": tx_fail}
        info["tcan_init"] = {"status": init, "notes": [t for bit, t in TCAN_INIT_BITS.items() if init & bit]}
        info["inputs"] = {"fault": [bool(inputs >> i & 1) for i in range(4)], "pg_5v": bool(inputs & 0x10), "can_int": bool(inputs & 0x20),
                          "drdy": [bool(inputs & 0x40), bool(inputs & 0x80)]}
    if len(p) >= 128:  # configuration fields (hubs that store a runtime config)
        ah, sh = struct.unpack_from("<QQ", p, 88)
        src, cfgst = p[104], p[105]
        rx_frames, rx_msgs, rx_bad, rx_over, tx_fail = struct.unpack_from("<IIIII", p, 108)
        rej = cfgst & 0x0F
        info["config"] = {"active_hash": f"{ah:016x}", "stored_hash": f"{sh:016x}" if sh else None,
                          "active_source": ["default", "flash"][src] if src < 2 else src,
                          "rejected": CFG_STATUS.get(rej, str(rej)) if rej else None, "default_unusable": bool(cfgst & 0x80)}
        info["canlink"] = {"rx_frames": rx_frames, "messages": rx_msgs, "dropped": rx_bad, "rx_overrun": rx_over, "tx_failures": tx_fail}
    if len(p) >= 152:  # per-AFE acquisition counters
        sa, sb, ea, eb, ra, rb = struct.unpack_from("<IIIIII", p, 128)
        info["afe"]["per_afe"] = {"samples": [sa, sb], "drdy_edges": [ea, eb], "resyncs": [ra, rb]}
    return info


def parse_info(p: bytes) -> dict:
    (proto, blob, fw, up, ah, sh, src, cm4, maxch, maxblob, chans) = struct.unpack_from("<BBHIQQBBHHH", p)
    info = {"device_type": "head", "proto_version": proto, "blob_version": blob, "fw_version": f"{fw >> 8}.{fw & 0xFF}", "uptime_ms": up,
            "active_hash": f"{ah:016x}", "stored_hash": f"{sh:016x}" if sh else None,
            "active_source": ["default", "flash", "uploaded"][src] if src < 3 else src, "cm4_alive": bool(cm4),
            "max_channels": maxch, "max_blob": maxblob, "channel_count": chans, "uid": p[32:44].hex(),
            "fdcan_kernel_hz": struct.unpack_from("<I", p, 44)[0] if len(p) >= 48 else None}
    if len(p) >= 80:  # appended by newer head firmware
        st, rc, tries, crc_fail, overflow, desync, ring_over, uart_err, tx_fail = struct.unpack_from("<BBHIIIIII", p, 48)
        init = p[76]
        info["cm4"] = {"buses_down": bool(st & 1), "boot_apply_pending": bool(st & 2), "boot_apply_rc": ERRORS.get(rc, rc) if rc else None,
                       "boot_apply_tries": tries}
        info["link"] = {"crc_failures": crc_fail, "oversize_frames": overflow, "desyncs": desync, "rx_overflow": ring_over,
                        "uart_errors": uart_err, "tx_failures": tx_fail}
        info["tcan_init"] = {"status": init, "notes": [t for bit, t in HEAD_TCAN_INIT_BITS.items() if init & bit]}
    return info


def parse_live(p: bytes) -> dict:
    first, count = struct.unpack_from("<HH", p)
    vals = [struct.unpack_from("<IB", p, 4 + 5 * i) for i in range(count)]
    return {"first": first, "raw": [v for v, _ in vals], "stale": [bool(f & 1) for _, f in vals],
            "no_sensor": [bool(f & 2) for _, f in vals]}


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
    out = {"buses": buses, "hub_lost": hub_lost}
    if off < len(p) and p[off] == nbus and off + 1 + 8 * nbus <= len(p):  # transmit counters (newer firmware)
        for i, b in enumerate(buses):
            b["tx_frames"], b["tx_dropped"] = struct.unpack_from("<II", p, off + 1 + 8 * i)
        off += 1 + 8 * nbus
        if off + 12 <= len(p):  # global receive-path counters (newest firmware)
            out["rx_nobus"], out["rx_ring_overrun"], out["rx_ring_high"] = struct.unpack_from("<III", p, off)
    return out


def parse_sniff(p: bytes) -> dict:
    bus, flags, cid, ts, ln = struct.unpack_from("<BBIIB", p)
    return {"bus": bus, "extended": bool(flags & 1), "fd": bool(flags & 2), "id": cid, "ts_ms": ts, "data": p[11:11 + ln]}
