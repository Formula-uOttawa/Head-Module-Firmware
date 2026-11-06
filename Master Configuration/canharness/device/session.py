"""One shared connection to a head or hub module for the web app (serialised; the UI polls). Connected to a head, a hub on the
CAN bus can be selected as the target: its requests are then relayed through the head (HUB_RELAY)."""

from __future__ import annotations

import struct
import threading
from collections import deque
from typing import Callable, Optional

from .. import blob as blobmod
from .client import DeviceClient, HubViaHead, SerialTransport, Transport
from .messages import DeviceError, HubTimeout, WrongDevice


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
        self.target: Optional[int] = None  # hub id reached through the connected head; None = the connected board itself

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
            self.client, self.port, self.target = None, None, None
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
        """What the requests currently talk to: the selected hub (through the head) or the connected board."""
        if not self.client:
            return None
        return "hub" if self.target is not None else self.client.device_type

    @property
    def hub_id(self) -> Optional[int]:
        """Hub id the next config upload is meant for: the selected hub, or the id a directly connected hub reports."""
        if self.target is not None:
            return self.target
        if self.client and self.client.device_type == "hub":
            return self.info().get("hub_id")
        return None

    def _head(self, what: str) -> DeviceClient:
        """The connected head itself (not a hub behind it) for head-only features."""
        c = self._c()
        if self.target is not None or c.device_type != "head":
            raise WrongDevice(what, self.device_type)
        return c

    def _ops(self) -> DeviceClient:
        """Client for config / info / live requests: the tunnel to the selected hub, or the connected board."""
        c = self._c()
        return HubViaHead(c, self.target) if self.target is not None else c

    # -- hubs behind the head ---------------------------------------------------------------------------------
    def scan_hubs(self, ids: list[int]) -> list[dict]:
        """Ask each hub id (through the head) for its IDENT: [{hub_id, found, fw_version, uid, error}]."""
        with self.lock:
            head = self._head("hub scan")
            out = []
            for i in ids:
                try:
                    r = head.relay(i, 0x07, b"", timeout=0.3)
                    from . import messages as m
                    ident = m.parse_ident(r)
                    out.append({"hub_id": i, "found": True, "device_type": ident["device_type"], "fw_version": ident["fw_version"], "uid": ident["uid"],
                                "reported_id": ident["hub_id"]})
                except HubTimeout:
                    out.append({"hub_id": i, "found": False})
            return out

    def select_hub(self, hub_id: Optional[int]) -> Optional[dict]:
        """Target a hub through the head (verified with INFO), or None to talk to the connected board again."""
        with self.lock:
            self._cfg_cache.clear()
            if hub_id is None:
                self.target = None
                return None
            c = self._head("hub selection")
            info = HubViaHead(c, hub_id).info()  # raises HubTimeout if nothing answers; the target is only set on success
            self.target = hub_id
            return info

    def _c(self) -> DeviceClient:
        if self.client is None:
            raise ConnectionError("not connected")
        return self.client

    # -- operations -------------------------------------------------------------------------------------------
    def info(self) -> dict:
        with self.lock:
            return self._ops().info()

    def status(self, since_text: int = 0) -> dict:
        """Cheap periodic health check: INFO (proves the device answers), bus stats, and any new text lines."""
        with self.lock:
            c = self._ops()
            info = c.info()
            try:
                stats = c.stats() if c.device_type == "head" else None
            except Exception:  # e.g. CM4 not running yet; INFO alone proves the link, so never fail the poll over stats
                stats = None
            return {"port": self.port, "target_hub": self.target, "info": info, "stats": stats, "text_last": self.text_seq,
                    "text": [t for t in self.text if t["seq"] > since_text]}

    def ascii(self, line: str) -> dict:
        """Send one legacy ASCII command and collect the reply lines (waits until the device goes quiet)."""
        with self.lock:
            c = self._c()
            before = self.text_seq
            if self.target is not None:  # a hub behind the head: the console runs on the hub (HUB_ASCII over the CAN relay)
                from . import messages as m
                text = HubViaHead(c, self.target).request(m.HUB_ASCII, line.strip().encode("ascii", "replace")[:120], timeout=2.0)
                for l in text.decode("ascii", "replace").replace("\r", "").split("\n"):
                    if l.strip():
                        self._on_text(l)
                return {"lines": [t for t in self.text if t["seq"] > before], "last": self.text_seq}
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
            return self._ops().upload_config(blob, apply=apply, store=store)

    def download(self, source: str = "active") -> bytes:
        with self.lock:
            return self._ops().read_config(source)

    def clear(self, revert: bool) -> None:
        with self.lock:
            self._ops().clear_config(revert)
            self._cfg_cache.clear()

    def reboot(self) -> None:
        with self.lock:
            self._ops().reboot()
            if self.target is None:  # a relayed hub reboot leaves the head connected
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
            c = self._ops()
            info = c.info()
            if c.device_type == "hub":  # raw value per connector-pin slot (slot = pin - 1); names come from the project
                r = c.live(0, 24)
                return {"device_type": "hub", "slots": [{"slot": i, "value": v, "stale": r["stale"][i], "no_sensor": r["no_sensor"][i]}
                                                        for i, v in enumerate(r["raw"])]}
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
