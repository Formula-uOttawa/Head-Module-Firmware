"""Hub module link: real hub_proto.c + hframe.c behind the same client/web stack as the head."""
import shutil
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from canharness.device import messages as m
from canharness.device.client import DeviceClient
from canharness.model import Hub, Project
from canharness.web import create_app

from test_device import ProcTransport
from test_head import build_sim, sample_project

ROOT = Path(__file__).resolve().parents[2]
HUB_CORE = ROOT.parent / "SensorHubV1" / "Core"
pytestmark = [pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler"),
              pytest.mark.skipif(not HUB_CORE.exists(), reason="SensorHubV1 not next to the head project")]


def test_hub_hframe_is_a_copy_of_common():
    for rel in ("Src/hframe.c", "Inc/hframe.h"):
        assert (HUB_CORE / rel).read_bytes() == (ROOT / "Common" / rel).read_bytes(), f"{rel} drifted from Common; re-copy it"


@pytest.fixture
def hub_exe(tmp_path):
    return build_sim(tmp_path, "hub_sim", ["hframe.c", "hub_proto.c"])


def test_ident_info_live(hub_exe):
    t = ProcTransport(hub_exe)
    texts = []
    c = DeviceClient(t, timeout=3.0, on_text=texts.append)
    try:
        ident = c.ident()
        assert ident == {"device_type": "hub", "proto_version": 1, "fw_version": "1.3", "hub_id": 2, "uid": bytes(range(0xA0, 0xAC)).hex()}
        c.device_type = ident["device_type"]
        info = c.info()
        assert info["hub_id"] == 2 and info["uptime_ms"] == 4321 and info["can_running"]
        assert info["sensors"] == {"afe_a": 5, "afe_b": 4, "digital": 1}
        assert info["can"] == {"tec": 3, "rec": 1, "bus_off": False, "error_passive": True}
        assert info["sysclk_hz"] == 160_000_000 and info["fdcan_kernel_hz"] == 80_000_000
        assert info["tx"] == {"ok": 100, "failed": 2, "skipped": 7, "invalid": 0} and info["tcan_int"] == 0x10
        live = c.live(0, 24)
        assert live["raw"] == [1000 + i for i in range(24)] and len(live["stale"]) == 24
        assert len(c.live(20, 48)["raw"]) == 4            # clamped at the 24 slots
        for mtype in (m.STATS_GET, m.CFG_BEGIN, m.SNIFF_SET):
            with pytest.raises(m.DeviceError, match="unsupported"):
                c.request(mtype)
        c.t.write(b"ping\n")
        c.info()
        assert texts == ["ascii:ping"]
    finally:
        t.close()


def test_web_detects_hub_and_guards_head_only_calls(tmp_path, hub_exe):
    proj = tmp_path / "p.json"
    sample_project().save(proj)
    app = create_app(proj, tmp_path / "out", transport_factory=lambda port, baud: ProcTransport(hub_exe))
    with TestClient(app) as c:
        r = c.post("/api/device/connect", json={"port": "sim"})
        assert r.status_code == 200 and r.json()["info"]["device_type"] == "hub"
        st = c.get("/api/device/status").json()
        assert st["info"]["hub_id"] == 2 and st["stats"] is None
        live = c.get("/api/device/live").json()
        assert live["device_type"] == "hub" and live["slots"][3] == {"slot": 3, "value": 1003}
        for url, kw in (("/api/device/upload", {"json": {"project": sample_project().model_dump(mode="json")}}),
                        ("/api/device/sniff", {"json": {"bus": 0, "enable": True}})):
            r = c.post(url, **kw)
            assert r.status_code == 409 and "hub module" in r.json()["detail"]
        assert c.get("/api/device/stats").status_code == 409
        assert c.get("/api/device/config").status_code == 409
        c.post("/api/device/disconnect")


def test_head_still_detected_with_and_without_ident(tmp_path):
    exe = build_sim(tmp_path, "proto_sim", ["head_cfg.c", "hframe.c", "hproto.c"])
    t = ProcTransport(exe)
    try:
        c = DeviceClient(t, timeout=3.0)
        ident = c.ident()
        assert ident["device_type"] == "head" and ident["hub_id"] is None and ident["fw_version"] == "1.2"
    finally:
        t.close()
