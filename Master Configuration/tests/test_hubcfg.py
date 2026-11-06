"""Hub configuration over CAN: the real head relay (hproto HUB_RELAY + head_relay.c + hcanlink.c) talking over a simulated CAN-FD
bus to the real hub receive path (hub_canlink.c, hub_proto.c, hub_cfg.c, hub_slots.c, hub_store.c) on a RAM model of the hub's flash
log (tests/c/relay_sim.c + hub_fake.c). The Python app side is the production client (DeviceClient.relay / HubViaHead)."""
import copy
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

from canharness import blob as headblob
from canharness import hubblob
from canharness.device import messages as m
from canharness.device.client import DeviceClient, HubViaHead
from canharness.generators.hub_c import generate_hub
from canharness.model import Channel

from test_device import ProcTransport
from test_head import hubch, sample_project

ROOT = Path(__file__).resolve().parents[2]
HUB_CORE = ROOT.parent / "SensorHubV1" / "Core"
TESTS_C = Path(__file__).parent / "c"
pytestmark = [pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler"),
              pytest.mark.skipif(not HUB_CORE.exists(), reason="SensorHubV1 not next to the head project")]


def test_hub_copies_of_shared_files_match_common():
    for rel in ("Src/hcanlink.c", "Inc/hcanlink.h"):
        assert (HUB_CORE / rel).read_bytes() == (ROOT / "Common" / rel).read_bytes(), f"{rel} drifted from Common; re-copy it"


def test_hcanlink_unit(tmp_path):
    exe = tmp_path / "hcl_sim"
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", f"-I{ROOT / 'Common' / 'Inc'}", "-o", str(exe), str(TESTS_C / "hcl_sim.c"),
                        str(ROOT / "Common" / "Src" / "hcanlink.c"), str(ROOT / "Common" / "Src" / "hframe.c")], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.returncode == 0 and "SIM OK" in r.stdout, r.stdout


# ---- fixtures ----------------------------------------------------------------------------------------------------------------

def project(variant=0, extra_analog=False):
    p = sample_project()
    p.channels[0].name = f"Throttle {variant}"  # a different name changes the project hash, so the blob differs
    if extra_analog:
        p.channels.append(hubch(5, 3, raw_bytes=3, signed=True))
    return p


@pytest.fixture(scope="module")
def relay_exe(tmp_path_factory):
    d = tmp_path_factory.mktemp("relay")
    default_c = d / "hub_default_blob.c"
    default_c.write_text(generate_hub(project(), 0)["hub_default_blob.c"])
    common, hub = ROOT / "Common" / "Src", HUB_CORE / "Src"
    srcs = [common / f for f in ("hcanlink.c", "hframe.c", "hproto.c", "head_relay.c", "head_cfg.c")] + \
           [hub / f for f in ("hub_canlink.c", "hub_proto.c", "hub_cfg.c", "hub_slots.c", "hub_store.c")] + \
           [TESTS_C / "relay_sim.c", TESTS_C / "hub_fake.c", default_c]
    exe = d / "relay_sim"
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-D_GNU_SOURCE", f"-I{TESTS_C / 'hal_stub'}", f"-I{ROOT / 'Common' / 'Inc'}",
           f"-I{HUB_CORE / 'Inc'}", "-o", str(exe), *map(str, srcs)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return exe


class Sim:
    """A head (UART client) with one hub on the simulated bus; `flash` persists across power_cycle()."""

    def __init__(self, exe, flash):
        self.exe, self.flash, self.texts = exe, flash, []
        self._start()

    def _start(self):
        self.t = ProcTransport(self.exe, self.flash)
        self.head = DeviceClient(self.t, timeout=5.0, on_text=self.texts.append)
        self.head.device_type = "head"

    def hub(self, i=0):
        return HubViaHead(self.head, i)

    def power_cycle(self):
        self.t.close()
        self._start()

    def ctl(self, line):
        self.texts.clear()
        self.t.write(line.encode() + b"\n")
        for _ in range(40):
            self.head.pump(0.05)
            if self.texts:
                return self.texts[-1].removeprefix("ascii:")
        raise AssertionError(f"no answer to {line}")

    def close(self):
        self.t.close()


@pytest.fixture
def sim(relay_exe, tmp_path):
    s = Sim(relay_exe, tmp_path / "flash.bin")
    yield s
    s.close()


# ---- the relay and the hub's receive path ---------------------------------------------------------------------------------------

def test_relay_ident_info_live(sim):
    p = project()
    h = sim.hub(0)
    assert sim.head.ident()["device_type"] == "head"          # the head itself still answers
    ident = m.parse_ident(h.request(m.IDENT))
    assert ident["device_type"] == "hub" and ident["hub_id"] == 0 and ident["fw_version"] == "2.0" and ident["uid"] == bytes(range(0xB0, 0xBC)).hex()
    info = h.info()  # 128 bytes: three CAN fragments back to the head
    assert info["hub_id"] == 0 and info["sensors"] == {"afe_a": 1, "afe_b": 0, "digital": 3}
    assert info["config"]["active_source"] == "default" and info["config"]["active_hash"] == p.config_hash() and info["config"]["stored_hash"] is None
    assert info["config"]["rejected"] is None and not info["config"]["default_unusable"]
    assert info["canlink"]["messages"] >= 1 and info["canlink"]["dropped"] == 0
    live = h.live(0, 24)
    assert live["raw"] == [1000 + i for i in range(24)]
    assert sim.ctl("sim:filter") == "filter:08000000:1F000000:1:sent=%d" % int(sim.texts[-1].rsplit("sent=", 1)[1])  # CONFIG type, hub 0, one ext filter


def test_unknown_hub_times_out_and_bad_ids(sim):
    with pytest.raises(m.HubTimeout, match="hub 3 did not answer"):
        sim.hub(3).info()
    assert sim.hub(0).info()["hub_id"] == 0                    # nothing is stuck afterwards
    sim.ctl("sim:absent:1")
    with pytest.raises(m.HubTimeout):
        sim.hub(0).info()
    sim.ctl("sim:absent:0")
    assert sim.hub(0).info()["hub_id"] == 0
    with pytest.raises(m.DeviceError, match="out of range"):   # hub ids are 0..7
        sim.head.request(m.HUB_RELAY, struct.pack("<BH", 9, 100) + bytes([m.INFO, 1]))
    with pytest.raises(m.DeviceError, match="bad request"):    # no inner message
        sim.head.request(m.HUB_RELAY, struct.pack("<BH", 0, 100))


def test_lost_fragment_times_out_then_recovers(sim):
    h = sim.hub(0)
    h.request(m.CFG_BEGIN, struct.pack("<II", 100, 0))
    chunk = struct.pack("<I", 0) + bytes(range(100))           # 104 bytes: two fragments
    sim.ctl("sim:drop:2")                                       # the second fragment is lost on the bus
    with pytest.raises(m.HubTimeout):
        h.request(m.CFG_CHUNK, chunk)
    assert struct.unpack("<I", h.request(m.CFG_CHUNK, chunk))[0] == 100  # the retry works, the hub was not left half way
    assert h.info()["canlink"]["dropped"] >= 1                  # the abandoned message was counted


def test_duplicated_frames_are_harmless_or_dropped(sim):
    h = sim.hub(0)
    sim.ctl("sim:dup:1")
    assert h.info()["hub_id"] == 0                              # single fragment requests are simply answered twice
    h.request(m.CFG_BEGIN, struct.pack("<II", 200, 0))
    small = struct.pack("<I", 0) + bytes(range(30))             # one fragment: delivered twice, the retransmit rule absorbs it
    assert struct.unpack("<I", h.request(m.CFG_CHUNK, small))[0] == 30
    two = struct.pack("<I", 30) + bytes(range(20))              # two fragments f0 f0 f1 f1: still completes (restart at 0, then 1)
    assert struct.unpack("<I", h.request(m.CFG_CHUNK, two))[0] == 50
    big = struct.pack("<I", 50) + bytes(range(150))             # three fragments f0 f0 f1 f1 f2 f2: the repeated fragment breaks the message
    with pytest.raises(m.HubTimeout):
        h.request(m.CFG_CHUNK, big)
    sim.ctl("sim:dup:0")
    assert struct.unpack("<I", h.request(m.CFG_CHUNK, big))[0] == 200


def test_tx_fifo_full_loses_the_reply_not_the_hub(sim):
    h = sim.hub(0)
    sim.ctl("sim:txfull:1")
    with pytest.raises(m.HubTimeout):
        h.info()
    sim.ctl("sim:txfull:0")
    assert h.info()["canlink"]["tx_failures"] >= 1


def test_hub_console_over_can(sim):
    """HUB_ASCII runs a console line on the hub and returns the text it printed (multi fragment replies included)."""
    h = sim.hub(0)
    assert h.request(m.HUB_ASCII, b"ping") == b"console:ping\r\n"
    long_line = ("afe:probe:a:" + "x" * 100).encode()
    assert h.request(m.HUB_ASCII, long_line) == b"console:" + long_line + b"\r\n"     # reply spans two CAN frames
    for bad in (b"", b"y" * 121):
        with pytest.raises(m.DeviceError, match="bad request"):
            h.request(m.HUB_ASCII, bad)
    h.request(m.HUB_ASCII, b"reset")                                                   # the hub resets after the reply went out
    assert h.info()["hub_id"] == 0 and h.info()["uptime_ms"] < 1_000_000


# ---- configuration upload, storage, reboot ----------------------------------------------------------------------------------------

def blob_for(p, hub=0):
    return hubblob.build(p, hub)


def test_upload_store_apply_reboots_into_new_config(sim):
    p = project(1, extra_analog=True)
    blob = blob_for(p)
    h = sim.hub(0)
    r = h.upload_config(blob, apply=True, store=True)
    assert r == {"applied": True, "stored": True, "hash": p.config_hash(), "store_error": None}
    # the hub reset itself (process restarted): new config is active and stored
    info = h.info()
    assert info["config"]["active_source"] == "flash" and info["config"]["active_hash"] == p.config_hash() == info["config"]["stored_hash"]
    assert info["sensors"] == {"afe_a": 2, "afe_b": 0, "digital": 3} and info["uptime_ms"] < 1_000_000
    assert h.read_config("active") == blob and h.read_config("stored") == blob
    assert hubblob.parse(h.read_config("active"))["hub_id"] == 0
    sim.power_cycle()                                           # and it survives a real power cycle
    info = sim.hub(0).info()
    assert info["config"]["active_source"] == "flash" and info["config"]["active_hash"] == p.config_hash()


def test_store_without_apply_waits_for_reset(sim):
    base = sim.hub(0).info()["config"]["active_hash"]
    p = project(2)
    h = sim.hub(0)
    r = h.upload_config(blob_for(p), apply=False, store=True)
    assert r["applied"] is False and r["stored"] is True
    cfg = h.info()["config"]
    assert cfg["active_hash"] == base and cfg["stored_hash"] == p.config_hash() and cfg["active_source"] == "default"
    h.reboot()
    cfg = h.info()["config"]
    assert cfg["active_hash"] == p.config_hash() and cfg["active_source"] == "flash"


def test_commit_flag_rules_and_staging_errors(sim):
    h = sim.hub(0)
    blob = blob_for(project())
    h.request(m.CFG_BEGIN, struct.pack("<II", len(blob), zlib.crc32(blob)))
    for bad in (0, 1, 4, 3 | 8):                                # store bit missing / unknown bits
        with pytest.raises(m.DeviceError, match="bad request"):
            h.request(m.CFG_COMMIT, bytes([bad]))
    with pytest.raises(m.DeviceError, match="busy"):            # nothing uploaded yet
        h.request(m.CFG_COMMIT, bytes([m.COMMIT_STORE]))
    h.request(m.CFG_CHUNK, struct.pack("<I", 0) + blob[:50])
    with pytest.raises(m.DeviceError, match="busy"):            # incomplete
        h.request(m.CFG_COMMIT, bytes([m.COMMIT_STORE]))
    with pytest.raises(m.DeviceError, match="out of range"):    # a hole
        h.request(m.CFG_CHUNK, struct.pack("<I", 80) + blob[80:100])
    with pytest.raises(m.DeviceError, match="too big"):
        h.request(m.CFG_BEGIN, struct.pack("<II", 5000, 0))
    with pytest.raises(m.DeviceError, match="busy"):            # the failed BEGIN dropped the upload
        h.request(m.CFG_CHUNK, struct.pack("<I", 50) + blob[50:60])
    # transfer CRC mismatch
    h.request(m.CFG_BEGIN, struct.pack("<II", len(blob), zlib.crc32(blob) ^ 1))
    for off in range(0, len(blob), 200):
        h.request(m.CFG_CHUNK, struct.pack("<I", off) + blob[off:off + 200])
    with pytest.raises(m.DeviceError, match="crc mismatch"):
        h.request(m.CFG_COMMIT, bytes([m.COMMIT_STORE]))
    assert h.info()["config"]["stored_hash"] is None


def crafted(rows, hub_id=0, sections=2, extra=b""):
    """A hub blob with arbitrary sensor rows (valid CRC), to exercise the hub's validation."""
    body = struct.pack("<HHI", hubblob.SEC_META, 8, 1) + struct.pack("<B7x", hub_id)
    body += struct.pack("<HHI", hubblob.SEC_SENSORS, hubblob.SENSOR_SIZE, len(rows))
    for r in rows:
        body += struct.pack(hubblob.SENSOR_FMT, *r)
    body += extra
    head = struct.pack("<IHHIIQII", hubblob.MAGIC, 1, 32, 32 + len(body), sections, 0x1234, 0, 0)
    b = bytearray(head + body)
    b[28:32] = struct.pack("<I", zlib.crc32(bytes(b)))
    return bytes(b)


def row(fid=0x00200000, group=1, chan=0, stype=1, rate=2, off=4, ln=3, slot=0, regs=(0x0C, 0, 0x1D, 0, 0x0F, 0xFF, 0)):
    return (fid, group, chan, stype, rate, off, ln, slot, 0, *regs, 0)


@pytest.mark.parametrize("why,blob", [
    ("head blob", None),
    ("bad hub id", crafted([row()], hub_id=8)),
    ("counter bits in the id", crafted([row(fid=0x00201200)])),
    ("duplicate slot", crafted([row(), row(chan=1, off=7)])),
    ("duplicate input", crafted([row(slot=0), row(slot=1, off=7)])),
    ("overlapping bytes", crafted([row(), row(chan=1, slot=1, off=6)])),
    ("same id, other rate", crafted([row(), row(chan=1, slot=1, off=7, rate=1)])),
    ("offset inside the timestamp", crafted([row(off=2)])),
    ("past 64 bytes", crafted([row(off=62, ln=4)])),
    ("zero length", crafted([row(ln=0)])),
    ("digital with analog type", crafted([row(group=3, stype=1)])),
    ("input out of range", crafted([row(chan=10)])),
    ("slot out of range", crafted([row(slot=24)])),
    ("bad rate", crafted([row(rate=3)])),
    ("bad group", crafted([row(group=4)])),
    ("sections missing", None),
])
def test_invalid_blobs_are_rejected_not_stored(sim, why, blob):
    if blob is None:
        blob = headblob.build(sample_project()) if why == "head blob" else struct.pack("<IHHIIQII", hubblob.MAGIC, 1, 32, 32, 2, 0, 0, 0)  # header only
        b = bytearray(blob)
        b[28:32] = struct.pack("<I", zlib.crc32(bytes(b)))
        blob = bytes(b)
    h = sim.hub(0)
    with pytest.raises(m.DeviceError, match="invalid config|too big"):
        h.upload_config(blob, apply=True, store=True)
    info = h.info()
    assert info["config"]["stored_hash"] is None and info["config"]["active_source"] == "default" and info["uptime_ms"] > 0


def test_valid_crafted_blob_and_unknown_section_accepted(sim):
    extra = struct.pack("<HHI", 0x77, 4, 1) + b"\x01\x02\x03\x04"   # unknown sections are skipped (forward compatibility)
    blob = crafted([row(), row(group=2, chan=1, slot=1, off=7, stype=0, rate=1, fid=0x00400000), row(group=3, chan=2, stype=3, slot=20, off=4, ln=1, fid=0x00800000, rate=0)],
                   extra=extra, sections=3)
    h = sim.hub(0)
    r = h.upload_config(blob, apply=False, store=True)
    assert r["stored"] and r["hash"] == f"{0x1234:016x}"
    h.reboot()
    info = h.info()
    assert info["sensors"] == {"afe_a": 1, "afe_b": 1, "digital": 1} and info["config"]["active_source"] == "flash"


def test_identical_store_is_skipped_and_log_rotates(sim):
    h = sim.hub(0)
    hashes = []
    for i in range(6):                                          # more stores than the 4 slots: the ring wraps
        p = project(10 + i)
        h.upload_config(blob_for(p), apply=False, store=True)
        hashes.append(p.config_hash())
        assert h.info()["config"]["stored_hash"] == hashes[-1]
    sim.power_cycle()
    assert sim.hub(0).info()["config"]["stored_hash"] == hashes[-1]
    sim.ctl("sim:budget:0")                                     # power is gone: any flash access fails ...
    h = sim.hub(0)
    r = h.upload_config(blob_for(project(15)), apply=False, store=True)   # ... but the identical blob does not touch the flash
    assert r["stored"] is True
    with pytest.raises(m.DeviceError, match="flash error"):
        h.upload_config(blob_for(project(99)), apply=False, store=True)


def test_torn_store_keeps_the_previous_config(sim):
    h = sim.hub(0)
    a = project(20)
    h.upload_config(blob_for(a), apply=False, store=True)
    # the blob is 17 words, then the 4 word header (the 4th is blank): a power failure after N words, header word 3 completes it
    for budget in (0, 1, 3, 6, 17, 18, 19):
        b = project(30 + budget)
        sim.ctl(f"sim:budget:{budget}")
        with pytest.raises(m.DeviceError, match="flash error"):
            h.upload_config(blob_for(b), apply=False, store=True)
        sim.power_cycle()                                       # budget is reset by the restart
        h = sim.hub(0)
        cfg = h.info()["config"]
        assert cfg["stored_hash"] == a.config_hash() and cfg["active_hash"] == a.config_hash() and cfg["active_source"] == "flash", budget
    r = h.upload_config(blob_for(project(77)), apply=False, store=True)   # the torn pages are reused by the next store
    assert r["stored"] and h.info()["config"]["stored_hash"] == project(77).config_hash()


def test_clear_erases_stored_config(sim):
    h = sim.hub(0)
    default_hash = h.info()["config"]["active_hash"]
    h.upload_config(blob_for(project(40)), apply=False, store=True)
    h.clear_config(revert=True)
    assert h.info()["config"]["stored_hash"] is None
    with pytest.raises(m.DeviceError, match="unsupported"):     # nothing stored to read
        h.read_config("stored")
    h.reboot()
    cfg = h.info()["config"]
    assert cfg["active_source"] == "default" and cfg["active_hash"] == default_hash


def test_read_config_active_default_matches_the_deployed_blob(sim):
    assert sim.hub(0).read_config("active") == generate_hub(project(), 0)["hub_0.blob"]


def test_hub_id_can_be_reassigned_by_a_new_config(sim):
    """A blob for hub 5 makes the hub answer as hub 5 after the reset (and no longer as hub 0)."""
    p = project(50)
    p.hubs[0].hub_id = 5
    for c in p.channels:
        if c.source == "hub":
            c.hub = 5
    h0 = sim.hub(0)
    h0.upload_config(hubblob.build(p, 5), apply=True, store=True)
    assert sim.hub(5).info()["hub_id"] == 5
    with pytest.raises(m.HubTimeout):
        sim.hub(0).info()
    assert sim.ctl("sim:filter").startswith("filter:0D000000:1F000000")  # CONFIG type, hub 5


# ---- the web app on top of it -------------------------------------------------------------------------------------------------

def test_web_scan_select_upload_and_head_only_guards(relay_exe, tmp_path):
    from fastapi.testclient import TestClient
    from canharness.web import create_app
    proj = tmp_path / "p.json"
    project().save(proj)
    app = create_app(proj, tmp_path / "out", transport_factory=lambda port, baud: ProcTransport(relay_exe, tmp_path / "flash.bin"))
    p = project(60, extra_analog=True)
    body = {"project": p.model_dump(mode="json")}
    with TestClient(app) as c:
        r = c.post("/api/device/connect", json={"port": "sim"})
        assert r.status_code == 200 and r.json()["info"]["device_type"] == "head"
        scan = c.post("/api/device/hubs/scan", json={"ids": [0, 1]}).json()["hubs"]
        assert [(h["hub_id"], h["found"]) for h in scan] == [(0, True), (1, False)] and scan[0]["device_type"] == "hub" and scan[0]["fw_version"] == "2.0"
        # selecting a hub that is not there fails and leaves the head selected
        r = c.post("/api/device/target", json={"hub_id": 1})
        assert r.status_code == 409 and "hub 1 did not answer" in r.json()["detail"]
        assert c.get("/api/device/status").json()["target_hub"] is None
        # select hub 0: status, live and info now come from the hub
        r = c.post("/api/device/target", json={"hub_id": 0})
        assert r.status_code == 200 and r.json()["target_hub"] == 0 and r.json()["info"]["device_type"] == "hub"
        st = c.get("/api/device/status").json()
        assert st["target_hub"] == 0 and st["info"]["device_type"] == "hub" and st["stats"] is None
        assert c.get("/api/device/live").json()["slots"][2]["value"] == 1002
        # the console box talks to the hub, not the head
        r = c.post("/api/device/ascii", json={"line": "afe:probe:a"}).json()
        assert [x["line"] for x in r["lines"]] == ["console:afe:probe:a"]
        # head-only operations are refused while a hub is selected
        assert c.get("/api/device/stats").status_code == 409
        assert c.post("/api/device/sniff", json={"bus": 0, "enable": True}).status_code == 409
        # hub config upload: the hub blob for the selected hub is built from the posted project; "apply" resets the hub
        bad = c.post("/api/device/upload", json=dict(body, as_hub=3))
        assert bad.status_code == 422 and "hub 3 is not defined" in str(bad.json()["detail"])
        r = c.post("/api/device/upload", json=dict(body, apply=True, store=False))  # a hub always stores
        assert r.status_code == 200 and r.json()["applied"] and r.json()["stored"] and r.json()["hub_id"] == 0 and r.json()["project_hash"] == p.config_hash()
        info = c.get("/api/device/info").json()
        assert info["config"]["active_source"] == "flash" and info["config"]["active_hash"] == p.config_hash()
        cfg = c.get("/api/device/config?source=stored").json()
        assert cfg["hash"] == p.config_hash() and cfg["hub_id"] == 0 and cfg["counts"] == {"sensors": 5}
        # a relayed reboot keeps the head connected and the hub selected
        assert c.post("/api/device/reboot").status_code == 200
        assert c.get("/api/device/status").json()["target_hub"] == 0
        assert c.post("/api/device/clear", json={"revert": True}).status_code == 200
        # back to the head itself
        assert c.post("/api/device/target", json={"hub_id": None}).json()["target_hub"] is None
        assert c.get("/api/device/status").json()["info"]["device_type"] == "head"
        c.post("/api/device/disconnect")
