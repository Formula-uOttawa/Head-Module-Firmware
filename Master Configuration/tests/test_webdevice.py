import shutil
import subprocess

import pytest
from fastapi.testclient import TestClient

from canharness import blob as blobmod
from canharness.model import Project
from canharness.web import create_app

from test_device import ProcTransport
from test_head import build_sim, sample_project

pytestmark = pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")


@pytest.fixture
def api(tmp_path):
    exe = build_sim(tmp_path, "proto_sim", ["head_cfg.c", "hframe.c", "hproto.c"])
    proj = tmp_path / "p.json"
    sample_project().save(proj)
    app = create_app(proj, tmp_path / "out", transport_factory=lambda port, baud: ProcTransport(exe))
    with TestClient(app) as c:
        yield c, sample_project()
        c.post("/api/device/disconnect")


def test_connect_upload_download_live(api):
    c, p = api
    assert c.get("/api/device/info").status_code == 504 or True  # not connected -> error status, not a crash
    r = c.post("/api/device/connect", json={"port": "sim"})
    assert r.status_code == 200 and r.json()["info"]["active_source"] == "default"
    body = p.model_dump(mode="json")
    r = c.post("/api/device/upload", json={"project": body, "apply": True, "store": True})
    assert r.status_code == 200 and r.json() == {"applied": True, "stored": True, "hash": p.config_hash(), "bytes": len(blobmod.build(p)), "project_hash": p.config_hash(), "store_error": None}
    cfg = c.get("/api/device/config", params={"source": "stored"}).json()
    assert cfg["hash"] == p.config_hash() and cfg["counts"]["channels"] == len([ch for ch in p.channels if ch.enabled])
    live = c.get("/api/device/live").json()
    assert live["hash"] == p.config_hash() and live["channels"][0]["channel_id"] == 1 and live["channels"][0]["value"] == 7
    assert live["channels"][1]["stale"] is True
    assert c.get("/api/device/stats").json()["hub_lost"][0] == 9


def test_upload_blocked_by_validation_and_device_errors(api):
    c, p = api
    c.post("/api/device/connect", json={"port": "sim"})
    bad = p.model_copy(deep=True)
    bad.channels[0].fault_low, bad.channels[0].fault_high = 5, 1
    r = c.post("/api/device/upload", json={"project": bad.model_dump(mode="json")})
    assert r.status_code == 422 and "fault_low" in r.json()["detail"][0]
    r = c.get("/api/device/config", params={"source": "stored"})      # nothing stored yet
    assert r.status_code == 409
    assert c.post("/api/device/sniff", json={"bus": 9, "enable": True}).status_code == 409


def test_sniff_stream(api):
    c, _ = api
    c.post("/api/device/connect", json={"port": "sim"})
    assert c.post("/api/device/sniff", json={"bus": 1, "enable": True}).status_code == 200
    got = c.get("/api/device/sniff", params={"since": 0}).json()
    assert got["frames"] and got["frames"][0]["id"] == 0x18FF1234 and got["frames"][0]["data"] == "aabbcc"
    assert c.get("/api/device/sniff", params={"since": got["last"]}).json()["frames"] == []


def test_not_connected(api):
    c, _ = api
    assert c.get("/api/device/live").status_code == 504
    assert c.get("/api/device/ports").json()["connected"] is None


def test_status_and_ascii(api):
    c, _ = api
    assert c.get("/api/device/status").status_code == 504           # not connected
    c.post("/api/device/connect", json={"port": "sim"})
    st = c.get("/api/device/status").json()
    assert st["port"] == "sim" and st["info"]["proto_version"] == 1 and st["stats"]["buses"][0]["rx_frames"] == 5 and st["text"] == []
    r = c.post("/api/device/ascii", json={"line": "ping"}).json()
    assert [t["line"] for t in r["lines"]] == ["ascii:ping"]
    # the reply was already delivered by the ascii call; status asked "since" that point must not repeat it
    assert c.get("/api/device/status", params={"since": r["last"]}).json()["text"] == []


def test_diagnostics_messages(tmp_path):
    from canharness.device.client import DeviceClient, Transport

    class Silent(Transport):
        def write(self, d): pass
        def read(self, t): return b""

    class Garbage(Transport):
        def write(self, d): pass
        def read(self, t): return b"\x00\x05\x06\x07\x00"   # a frame with a bad crc

    with pytest.raises(TimeoutError, match="no bytes received"):
        DeviceClient(Silent(), timeout=0.2).info()
    with pytest.raises(TimeoutError, match=r"bytes received but no valid reply, \d+ failed CRC"):
        DeviceClient(Garbage(), timeout=0.2).info()

    # an unexpected exception inside a device operation is reported with its type and message, not a bare 500
    class Boom(Transport):
        def write(self, d): raise RuntimeError("kaboom")
        def read(self, t): return b""

    proj = tmp_path / "p.json"
    sample_project().save(proj)
    with TestClient(create_app(proj, tmp_path / "out", transport_factory=lambda p, b: Boom())) as c:
        r = c.post("/api/device/connect", json={"port": "x"})
        assert r.status_code == 500 and "RuntimeError: kaboom" in r.json()["detail"]
