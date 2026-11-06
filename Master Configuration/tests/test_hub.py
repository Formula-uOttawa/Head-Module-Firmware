"""Hub module link: real hub_proto.c + hframe.c behind the same client/web stack as the head."""
import shutil
import struct
import subprocess
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
    return build_sim(tmp_path, "hub_sim", ["hframe.c", "hub_proto.c", "hub_cfg.c"])


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
        assert {k: info["tx"][k] for k in ("ok", "failed", "skipped", "invalid")} == {"ok": 100, "failed": 2, "skipped": 7, "invalid": 0} and info["tcan_int"] == 0x10
        live = c.live(0, 24)
        assert live["raw"] == [1000 + i for i in range(24)] and len(live["stale"]) == 24
        assert [i for i, v in enumerate(live["stale"]) if v] == [5]        # flags bit0 = stale
        assert len(c.live(20, 48)["raw"]) == 4            # clamped at the 24 slots
        with pytest.raises(m.DeviceError, match="range"):  # first slot beyond the table is an error, not an empty reply
            c.live(24, 1)
        with pytest.raises(m.DeviceError, match="range"):
            c.live(0, 49)
        for mtype in (m.STATS_GET, m.SNIFF_SET):
            with pytest.raises(m.DeviceError, match="unsupported"):
                c.request(mtype)
        c.t.write(b"ping\n")
        c.info()
        assert texts == ["ascii:ping"]
        # REBOOT needs the 4 byte magic: anything else is refused and must not reach the platform
        for bad in (b"", b"\x00\x00\x00\x00", struct.pack("<I", m.REBOOT_MAGIC)[:3], struct.pack("<I", m.REBOOT_MAGIC) + b"\x00"):
            with pytest.raises(m.DeviceError, match="bad_request|BAD_REQUEST|bad request"):
                c.request(m.REBOOT, bad)
        assert "ascii:reboot-called" not in texts
        c.reboot()
        c.info()
        assert texts == ["ascii:ping", "ascii:reboot-called"]
    finally:
        t.close()


def test_info_payload_layout_and_appended_counters(hub_exe):
    """INFO keeps its first 56 bytes and appends the new counters (offsets documented in hub_proto.h)."""
    t = ProcTransport(hub_exe)
    try:
        c = DeviceClient(t, timeout=3.0)
        p = c.request(m.INFO)
        assert len(p) == 152
        u32 = lambda off: struct.unpack_from("<I", p, off)[0]
        assert p[0] == 1 and u32(4) == 4321 and p[8] == 2 and p[9] == 0x01 and p[10:13] == bytes([5, 4, 1])
        assert [u32(o) for o in (28, 32, 36, 40)] == [100, 2, 7, 0] and p[44:56] == bytes(range(0xA0, 0xAC))
        assert [u32(o) for o in (56, 60, 64, 68, 72, 76, 80)] == [11, 12, 13, 14, 15, 16, 17]
        assert p[84:88] == bytes([0x60, 0x3F, 0x15, 0x00])
        assert struct.unpack_from("<QQ", p, 88) == (0x1122334455667788, 0) and p[104] == 1 and p[105] == 0x83
        assert [u32(o) for o in (108, 112, 116, 120, 124)] == [21, 22, 23, 24, 25]
        assert [u32(o) for o in (128, 132, 136, 140, 144, 148)] == [31, 32, 33, 34, 35, 36]
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
        assert live["device_type"] == "hub" and live["slots"][3]["slot"] == 3 and live["slots"][3]["value"] == 1003 and live["slots"][3]["stale"] is False
        r = c.post("/api/device/upload", json={"project": sample_project().model_dump(mode="json")})
        assert r.status_code == 422 and "hub 2 is not defined in this project" in str(r.json()["detail"])  # the hub reported id 2
        r = c.post("/api/device/sniff", json={"bus": 0, "enable": True})
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


# ---- real hub firmware modules against a stub HAL (tests/c/hal_stub) ------------------------------------------------

def _build_hub_hal_sim(tmp_path, harness, sources, defines=()):
    """Compile tests/c/<harness>.c with hub Core sources and the host HAL stub; returns the executable."""
    exe = tmp_path / (harness + "".join(d.strip("-D").lower() for d in defines))
    tests_c = Path(__file__).parent / "c"
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", *defines, f"-I{tests_c / 'hal_stub'}", f"-I{HUB_CORE / 'Inc'}",
           "-o", str(exe), str(tests_c / f"{harness}.c"), *[str(HUB_CORE / "Src" / f) for f in sources]]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return exe


def _run_sim(exe, *args):
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and "SIM OK" in r.stdout, r.stdout + r.stderr


def _legacy_hub_blob(tmp_path):
    """Hub 0 blob of the rear-sensor project (tests/fixtures, a snapshot of system.json): the table the schedule/AFE simulations
    are written against."""
    from canharness import hubblob
    f = tmp_path / "hub0.blob"
    f.write_bytes(hubblob.build(Project.load(Path(__file__).parent / "fixtures" / "rear_hub_project.json"), 0))
    return f


def test_hub_scheduler_generated_config(tmp_path):
    """Staggered groups, 8 bit frame counter in id bits 15:8, sign extended low bytes, digital inputs, TX FIFO full handling."""
    _run_sim(_build_hub_hal_sim(tmp_path, "sched_sim", ["can_scheduler.c", "live_slots.c", "hub_cfg.c"]), _legacy_hub_blob(tmp_path))


def test_hub_scheduler_rejects_bad_config(tmp_path):
    """Overlapping bytes, id conflicts, counter bits in the id, bad slots and an overloaded tick are detected at init."""
    _run_sim(_build_hub_hal_sim(tmp_path, "sched_sim", ["can_scheduler.c", "live_slots.c", "hub_cfg.c"], ["-DSIM_CUSTOM"]))


def test_hub_afe_driver(tmp_path):
    """AFE A/B addressing, init sequence, sign extension, DRDY service, error / watchdog recovery, staleness flags."""
    _run_sim(_build_hub_hal_sim(tmp_path, "afe_sim", ["afe_ctrl.c", "live_slots.c", "hub_cfg.c"]), _legacy_hub_blob(tmp_path))


def test_hub_debug_sniff_queue(tmp_path):
    """The real debug_sniff.c (compiled with DEF_DEBUG): raw events queued in the ISR path, formatted in flush, bounded."""
    _run_sim(_build_hub_hal_sim(tmp_path, "sniff_sim", ["debug_sniff.c"], ["-DDEF_DEBUG"]))
