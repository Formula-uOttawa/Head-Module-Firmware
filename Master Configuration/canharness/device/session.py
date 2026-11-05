"""One shared connection to a head module for the web app (serialised; the UI polls)."""

from __future__ import annotations

import struct
import threading
from collections import deque
from typing import Callable, Optional

from .. import blob as blobmod
from .client import DeviceClient, SerialTransport, Transport
from .messages import DeviceError, WrongDevice


def list_ports() -> list[dict]:
    try:
        from serial.tools import list_ports as lp
        ports = sorted(lp.comports(), key=lambda p: p.device)
        devices = {p.device for p in ports}
        # macOS lists every USB serial port twice; the tty.* (call-in) twin can block or behave oddly, cu.* is the one to use
        ports = [p for p in ports if not (p.device.startswith("/dev/tty.") and p.device.replace("/dev/tty.", "/dev/cu.", 1) in devices)]
        return [{"device": p.device, "description": p.description} for p in ports]
    except Exception:  # pyserial missing or no permission to enumerate
        return []


class DeviceSession:
    def __init__(self, factory: Optional[Callable[[str, int], Transport]] = None):
        self.factory = factory or (lambda port, baud: SerialTransport(port, baud))
        self.lock = threading.RLock()
        self.client: Optional[DeviceClient] = None
        self.port: Optional[str] = None
        self.sniff_frames: deque = deque(maxlen=1000)
        self.sniff_seq = 0
        self.text: deque = deque(maxlen=500)  # unsolicited / ASCII lines from the device
        self.text_seq = 0
        self._cfg_cache: dict[str, dict] = {}

    # -- connection -------------------------------------------------------------------------------------------
    def connect(self, port: str, baud: int = 115200) -> dict:
        with self.lock:
            self.disconnect()
            t = self.factory(port, baud)
            self.client = DeviceClient(t, timeout=1.5, on_sniff=self._on_sniff, on_text=self._on_text)
            try:
                self.client.device_type = self.client.ident()["device_type"]
                info = self.client.info()
            except Exception:
                self.disconnect()
                raise
            self.port = port
            return info

    def disconnect(self) -> None:
        with self.lock:
            if self.client:
                try: self.client.t.close()
                except Exception: pass
            self.client, self.port = None, None
            self._cfg_cache.clear()

    def _on_text(self, line: str) -> None:
        self.text_seq += 1
        self.text.append({"seq": self.text_seq, "line": line})

    def _on_sniff(self, frame: dict) -> None:
        self.sniff_seq += 1
        frame = dict(frame, seq=self.sniff_seq, data=frame["data"].hex())
        self.sniff_frames.append(frame)

    @property
    def device_type(self) -> Optional[str]:
        return self.client.device_type if self.client else None

    def _head(self, what: str) -> DeviceClient:
        c = self._c()
        if c.device_type != "head":
            raise WrongDevice(what, c.device_type)
        return c

    def _c(self) -> DeviceClient:
        if self.client is None:
            raise ConnectionError("not connected")
        return self.client

    # -- operations -------------------------------------------------------------------------------------------
    def info(self) -> dict:
        with self.lock:
            return self._c().info()

    def status(self, since_text: int = 0) -> dict:
        """Cheap periodic health check: INFO (proves the device answers), bus stats, and any new text lines."""
        with self.lock:
            c = self._c()
            info = c.info()
            try:
                stats = c.stats() if c.device_type == "head" else None
            except Exception:  # e.g. CM4 not running yet; INFO alone proves the link, so never fail the poll over stats
                stats = None
            return {"port": self.port, "info": info, "stats": stats, "text_last": self.text_seq,
                    "text": [t for t in self.text if t["seq"] > since_text]}

    def ascii(self, line: str) -> dict:
        """Send one legacy ASCII command and collect the reply lines (waits until the device goes quiet)."""
        with self.lock:
            c = self._c()
            before = self.text_seq
            c.t.write(line.strip().encode("ascii", "replace") + b"\n")
            quiet = 0
            for _ in range(10):
                n = self.text_seq
                c.pump(0.1)
                quiet = quiet + 1 if self.text_seq == n else 0
                if self.text_seq > before and quiet >= 2:
                    break
            return {"lines": [t for t in self.text if t["seq"] > before], "last": self.text_seq}

    def upload(self, blob: bytes, apply: bool, store: bool) -> dict:
        with self.lock:
            self._cfg_cache.clear()
            return self._head("config upload").upload_config(blob, apply=apply, store=store)

    def download(self, source: str = "active") -> bytes:
        with self.lock:
            return self._head("config read").read_config(source)

    def clear(self, revert: bool) -> None:
        with self.lock:
            self._head("config clear").clear_config(revert)
            self._cfg_cache.clear()

    def reboot(self) -> None:
        with self.lock:
            self._c().reboot()
            self.disconnect()

    def stats(self) -> dict:
        with self.lock:
            return self._head("bus statistics").stats()

    def _active_channels(self, info: dict) -> list[tuple]:
        key = info["active_hash"]
        if key not in self._cfg_cache:
            self._cfg_cache = {key: blobmod.parse(self._c().read_config("active"))}
        return self._cfg_cache[key]["channels"]

    def live(self) -> dict:
        """Current value of every channel of the active config: [{channel_id, value, stale}]."""
        with self.lock:
            c = self._c()
            info = c.info()
            if c.device_type == "hub":  # raw value per connector-pin slot (slot = pin - 1); names come from the project
                r = c.live(0, 24)
                return {"device_type": "hub", "slots": [{"slot": i, "value": v} for i, v in enumerate(r["raw"])]}
            chans = self._active_channels(info)
            rows, slot = [], 0
            while slot < len(chans):
                r = c.live(slot, min(48, len(chans) - slot))
                if not r["raw"]:
                    break
                for raw, stale in zip(r["raw"], r["stale"]):
                    cid, vtype = chans[slot][0], chans[slot][1]
                    value = (struct.unpack("<f", struct.pack("<I", raw))[0] if vtype == 2
                             else struct.unpack("<i", struct.pack("<I", raw))[0] if vtype == 1 else raw)
                    rows.append({"slot": slot, "channel_id": cid, "value": value, "stale": stale})
                    slot += 1
            return {"device_type": "head", "hash": info["active_hash"], "channels": rows}

    def sniff(self, bus: int, enable: bool) -> None:
        with self.lock:
            self._head("CAN sniffer").sniff(bus, enable)

    def sniff_poll(self, since: int) -> dict:
        with self.lock:
            if self.client:
                self.client.pump(0.05)
            return {"frames": [f for f in self.sniff_frames if f["seq"] > since], "last": self.sniff_seq}
