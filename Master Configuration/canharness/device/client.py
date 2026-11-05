"""Device client: talks the head protocol over any byte transport (serial now, UDP later)."""

from __future__ import annotations

import struct
import time
import zlib
from typing import Callable, Optional

from . import messages as m
from .framing import FrameParser, encode_frame


class Transport:
    def write(self, data: bytes) -> None: raise NotImplementedError
    def read(self, timeout: float) -> bytes: raise NotImplementedError
    def close(self) -> None: pass


class SerialTransport(Transport):
    def __init__(self, port: str, baud: int = 115200):
        import serial  # pyserial
        self.ser = serial.Serial(port, baud, timeout=0)

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def read(self, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.ser.read(self.ser.in_waiting or 1)
            if data:
                return data
            time.sleep(0.002)
        return b""

    def close(self) -> None:
        self.ser.close()


class DeviceClient:
    def __init__(self, transport: Transport, timeout: float = 1.0, on_text: Optional[Callable[[str], None]] = None,
                 on_sniff: Optional[Callable[[dict], None]] = None):
        self.t, self.timeout = transport, timeout
        self.parser = FrameParser()
        self.seq = 0
        self.on_text, self.on_sniff = on_text, on_sniff
        self.pending: list = []
        self.device_type = "head"  # set by ident() on connect

    def request(self, mtype: int, payload: bytes = b"", timeout: Optional[float] = None) -> bytes:
        self.seq = (self.seq + 1) & 0xFF or 1
        seq = self.seq
        self.t.write(encode_frame(mtype, seq, payload))
        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        nrx, bad0 = 0, self.parser.bad_frames
        while time.monotonic() < deadline:
            data = self.t.read(0.05)
            nrx += len(data)
            items = self.pending + self.parser.feed(data)
            self.pending = []
            for i, (kind, item) in enumerate(items):
                if kind == "text":
                    if self.on_text: self.on_text(item)
                    continue
                rtype, rseq, rpayload = item
                if rtype == m.SNIFF_FRAME:
                    if self.on_sniff: self.on_sniff(m.parse_sniff(rpayload))
                    continue
                if rseq != seq:
                    continue
                self.pending = items[i + 1:]  # keep what follows the reply for the next request
                if rtype == m.ERROR:
                    raise m.DeviceError(rpayload[0], rpayload[1])
                if rtype == (mtype | 0x80):
                    return rpayload
        bad = self.parser.bad_frames - bad0
        seen = ("no bytes received (wrong port or baud, device stopped, or another program is using the port?)" if nrx == 0 else
                f"{nrx} bytes received but no valid reply" + (f", {bad} failed CRC (baud rate or noise?)" if bad else ""))
        raise TimeoutError(f"device did not answer {m.name(mtype)}: {seen}")

    # --- high level -----------------------------------------------------------------------------------------
    def ident(self) -> dict:
        """Which board is this? Head firmware from before IDENT existed answers 'unsupported'; that means a head."""
        try:
            return m.parse_ident(self.request(m.IDENT))
        except m.DeviceError as e:
            if e.code not in (1, 8):
                raise
            return {"device_type": "head", "proto_version": 1, "fw_version": None, "hub_id": None, "uid": None}

    def info(self) -> dict:
        if getattr(self, "device_type", "head") == "hub":
            return m.parse_hub_info(self.request(m.INFO))
        return m.parse_info(self.request(m.INFO))

    def upload_config(self, blob: bytes, apply: bool = True, store: bool = False,
                      progress: Optional[Callable[[int, int], None]] = None) -> dict:
        crc = zlib.crc32(blob)
        chunk = struct.unpack("<H", self.request(m.CFG_BEGIN, struct.pack("<II", len(blob), crc)))[0]
        chunk = min(chunk, m.CHUNK_MAX)
        for off in range(0, len(blob), chunk):
            self.request(m.CFG_CHUNK, struct.pack("<I", off) + blob[off:off + chunk])
            if progress: progress(min(off + chunk, len(blob)), len(blob))
        flags = (m.COMMIT_APPLY if apply else 0) | (m.COMMIT_STORE if store else 0)
        p = self.request(m.CFG_COMMIT, bytes([flags]), timeout=20.0)  # a flash erase can take seconds
        applied, stored, h = struct.unpack("<BBQ", p)
        return {"applied": bool(applied), "stored": bool(stored), "hash": f"{h:016x}"}

    def read_config(self, source: str = "active") -> bytes:
        src, out, total, off = (0 if source == "active" else 1), bytearray(), None, 0
        while total is None or off < total:
            p = self.request(m.CFG_READ, struct.pack("<BIH", src, off, 232))
            total, poff = struct.unpack_from("<II", p)
            data = p[8:]
            if poff != off or (not data and off < total):
                raise m.DeviceError(m.CFG_READ, 9)
            out += data
            off += len(data)
        return bytes(out)

    def clear_config(self, revert: bool = True) -> None:
        self.request(m.CFG_CLEAR, bytes([1 if revert else 0]), timeout=20.0)

    def live(self, first: int = 0, count: int = 48) -> dict:
        return m.parse_live(self.request(m.LIVE_GET, struct.pack("<HH", first, count)))

    def stats(self) -> dict:
        return m.parse_stats(self.request(m.STATS_GET))

    def sniff(self, bus: int, enable: bool) -> None:
        self.request(m.SNIFF_SET, bytes([bus, int(enable)]))

    def reboot(self) -> None:
        self.request(m.REBOOT)


def _pump(self, timeout: float = 0.05) -> None:
    """Read whatever the device sent unprompted (sniffed frames, text) without issuing a request."""
    for kind, item in self.pending + self.parser.feed(self.t.read(timeout)):
        if kind == "text":
            if self.on_text: self.on_text(item)
        elif item[0] == m.SNIFF_FRAME and self.on_sniff:
            self.on_sniff(m.parse_sniff(item[2]))
    self.pending = []


DeviceClient.pump = _pump
