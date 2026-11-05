import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

from canharness import blob as blobmod
from canharness.device import messages as m
from canharness.device.client import DeviceClient, Transport
from canharness.device.framing import FrameParser, cobs_decode, cobs_encode, crc16, encode_frame
from canharness.generators.head_c import head_tables

from test_head import build_sim, sample_project

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / "Common"
HAVE_CC = shutil.which("cc") is not None


# ---- framing -----------------------------------------------------------------------------------------------------

def test_crc16_check_value():
    assert crc16(b"123456789") == 0x29B1  # CRC-16/CCITT-FALSE


@pytest.mark.parametrize("data", [b"", b"\x00", b"\x00\x00", b"abc", bytes(range(256)), bytes(300), b"\x01" * 254, b"\x01" * 255, b"\x11\x00\x22"])
def test_cobs_roundtrip_and_no_zero(data):
    enc = cobs_encode(data)
    assert 0 not in enc and cobs_decode(enc) == data


def test_parser_frames_text_and_corruption():
    p = FrameParser()
    good = encode_frame(0x01, 5, b"hi")
    bad = bytearray(encode_frame(0x01, 6, b"hi")); bad[3] ^= 0x55
    stream = b"ping\n" + good + bytes(bad) + b"\x00\x00" + good + b"reset\r\n"
    items = list(p.feed(stream))
    assert items == [("text", "ping"), ("frame", (1, 5, b"hi")), ("frame", (1, 5, b"hi")), ("text", "reset")]


# ---- blob --------------------------------------------------------------------------------------------------------

def test_blob_roundtrip_matches_tables():
    p = sample_project()
    blob = blobmod.build(p)
    parsed = blobmod.parse(blob)
    tables = head_tables(p)
    assert parsed["config_hash"] == int(p.config_hash(), 16)
    for name in blobmod.LIMITS:
        assert len(parsed[name]) == len(tables[name]), name
    assert parsed["hub_frames"][0][0] == tables["hub_frames"][0][0]
    assert parsed["rx_signals"][0][-2] == pytest.approx(0.1, rel=1e-6)


def test_blob_rejects_corruption():
    blob = bytearray(blobmod.build(sample_project()))
    blob[100] ^= 1
    with pytest.raises(ValueError, match="crc"):
        blobmod.parse(bytes(blob))


@pytest.mark.skipif(not HAVE_CC, reason="no host C compiler")
def test_firmware_parser_agrees_and_rejects(tmp_path):
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])
    p = sample_project()
    blob = blobmod.build(p)
    f = tmp_path / "ok.blob"; f.write_bytes(blob)
    line = subprocess.run([str(exe), str(f)], capture_output=True, text=True, input="").stdout.splitlines()[0]
    t = head_tables(p)
    assert line.startswith("cfg 0 ok") and f"ch={len(t['channels'])} " in line and f"hf={len(t['hub_frames'])} " in line
    assert f"rs={len(t['rx_signals'])} " in line and f"hash={p.config_hash()}" in line and "hubbus=0" in line
    # every single-byte corruption in the header/body is rejected by the firmware parser (crc), none crashes
    for i in (0, 5, 9, 14, 20, 28, 40, len(blob) - 1):
        bad = bytearray(blob); bad[i] ^= 0x40
        (tmp_path / "bad.blob").write_bytes(bytes(bad))
        out = subprocess.run([str(exe), str(tmp_path / "bad.blob")], capture_output=True, text=True, input="")
        assert out.stdout.startswith("cfg ") and not out.stdout.startswith("cfg 0 ok"), i
    # truncated
    (tmp_path / "short.blob").write_bytes(blob[:20])
    assert "cfg 1" in subprocess.run([str(exe), str(tmp_path / "short.blob")], capture_output=True, text=True, input="").stdout


# ---- protocol against the real firmware dispatcher -----------------------------------------------------------------

class ProcTransport(Transport):
    def __init__(self, exe):
        self.p = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        import os, select
        self._select, self._os = select, os

    def write(self, data): self.p.stdin.write(data)

    def read(self, timeout):
        r, _, _ = self._select.select([self.p.stdout], [], [], timeout)
        return self._os.read(self.p.stdout.fileno(), 4096) if r else b""

    def close(self):
        self.p.stdin.close(); self.p.wait(timeout=5)


@pytest.fixture
def dev(tmp_path):
    if not HAVE_CC:
        pytest.skip("no host C compiler")
    exe = build_sim(tmp_path, "proto_sim", ["head_cfg.c", "hframe.c", "hproto.c"])
    t = ProcTransport(exe)
    texts, sniffs = [], []
    yield DeviceClient(t, timeout=3.0, on_text=texts.append, on_sniff=sniffs.append), texts, sniffs
    t.close()


def test_info_and_ascii_coexist(dev):
    c, texts, _ = dev
    c.t.write(b"ping\n")
    info = c.info()
    assert info["proto_version"] == 1 and info["fw_version"] == "1.2" and info["active_source"] == "default"
    assert info["uid"] == bytes(range(12)).hex() and info["max_blob"] == 16384 and info["fdcan_kernel_hz"] == 80_000_000
    c.t.write(b"hello world\n")
    c.info()
    assert texts == ["ascii:ping", "ascii:hello world"]


def test_config_upload_download_roundtrip(dev):
    c, _, _ = dev
    blob = blobmod.build(sample_project())
    prog = []
    res = c.upload_config(blob, apply=True, store=True, progress=lambda a, b: prog.append(a))
    assert res == {"applied": True, "stored": True, "hash": sample_project().config_hash(), "store_error": None} and prog[-1] == len(blob)
    assert c.read_config("active") == blob and c.read_config("stored") == blob
    info = c.info()
    assert info["active_source"] == "flash" and info["stored_hash"] == sample_project().config_hash()
    c.clear_config(revert=True)
    assert c.info()["stored_hash"] is None and c.info()["active_source"] == "default"


def test_upload_errors(dev):
    c, _, _ = dev
    blob = blobmod.build(sample_project())
    # too big
    with pytest.raises(m.DeviceError) as e:
        c.request(m.CFG_BEGIN, struct.pack("<II", 1 << 20, 0))
    assert e.value.code == 3
    # chunk without begin / out of order
    c.request(m.CFG_BEGIN, struct.pack("<II", len(blob), zlib.crc32(blob)))
    with pytest.raises(m.DeviceError) as e:
        c.request(m.CFG_CHUNK, struct.pack("<I", 10) + b"x")
    assert e.value.code == 9
    # commit before complete
    with pytest.raises(m.DeviceError) as e:
        c.request(m.CFG_COMMIT, b"\x01")
    assert e.value.code == 2
    # wrong crc
    c.request(m.CFG_BEGIN, struct.pack("<II", len(blob), 0x1234))
    for off in range(0, len(blob), 240):
        c.request(m.CFG_CHUNK, struct.pack("<I", off) + blob[off:off + 240])
    with pytest.raises(m.DeviceError) as e:
        c.request(m.CFG_COMMIT, b"\x01")
    assert e.value.code == 4
    # structurally invalid blob with a valid crc: flip a byte then re-crc
    bad = bytearray(blob); bad[12] = 200; bad[28:32] = b"\0\0\0\0"; bad[28:32] = struct.pack("<I", zlib.crc32(bytes(bad)))
    with pytest.raises(m.DeviceError) as e:
        c.upload_config(bytes(bad))
    assert e.value.code == 5
    # unknown message type
    with pytest.raises(m.DeviceError) as e:
        c.request(0x55)
    assert e.value.code == 8


def test_live_stats_sniff(dev):
    c, _, sniffs = dev
    live = c.live(2, 3)
    assert live["first"] == 2 and live["raw"] == [2007, 3007, 4007] and live["stale"] == [False, True, False]
    st = c.stats()
    assert st["buses"][0] == {"rx_frames": 5, "rx_matched": 4, "rx_dropped": 1, "fifo_lost": 2, "bus_off": False, "error_passive": True}
    assert st["hub_lost"][0] == 9 and len(st["hub_lost"]) == 8
    c.sniff(1, True)
    c.info()  # the unsolicited frame is delivered while waiting for the next reply
    assert sniffs and sniffs[0] == {"bus": 1, "extended": True, "fd": False, "id": 0x18FF1234, "ts_ms": 77, "data": b"\xaa\xbb\xcc"}
    with pytest.raises(m.DeviceError):
        c.sniff(9, True)


def test_store_failure_after_apply_is_reported_not_raised(tmp_path, monkeypatch):
    """A flash failure after a successful apply comes back as a normal reply with stored=False and a store_error."""
    monkeypatch.setenv("HPROTO_SIM_FAIL_STORE", "1")
    exe = build_sim(tmp_path, "proto_sim", ["head_cfg.c", "head_decode.c", "head_slots.c", "hframe.c", "hproto.c"])
    t = ProcTransport(exe)
    try:
        c = DeviceClient(t, timeout=3.0)
        res = c.upload_config(blobmod.build(sample_project()), apply=True, store=True)
        assert res["applied"] is True and res["stored"] is False and res["store_error"] == "flash error"
    finally:
        t.close()


def test_reboot_requires_the_magic(dev):
    c, _, _ = dev
    with pytest.raises(m.DeviceError, match="bad request"):
        c.request(m.REBOOT)                          # no payload
    with pytest.raises(m.DeviceError, match="bad request"):
        c.request(m.REBOOT, struct.pack("<I", 0x12345678))
    c.reboot()                                       # the client sends the magic


def test_parse_extended_info_and_stats():
    base = struct.pack("<BBHIQQBBHHH12sI", 1, 1, 0x0101, 5000, 0x11, 0x22, 1, 1, 256, 16384, 3, bytes(range(12)), 80_000_000)
    ext = struct.pack("<BBHIIIIII", 0x02, 5, 4, 1, 2, 3, 4, 5, 6) + bytes([0x24, 0, 0, 0])
    info = m.parse_info(base + ext)
    assert info["cm4"] == {"buses_down": False, "boot_apply_pending": True, "boot_apply_rc": "invalid config", "boot_apply_tries": 4}
    assert info["link"] == {"crc_failures": 1, "oversize_frames": 2, "desyncs": 3, "rx_overflow": 4, "uart_errors": 5, "tx_failures": 6}
    assert info["tcan_init"]["status"] == 0x24 and "warm start" in info["tcan_init"]["notes"] and "SBC init failed" in info["tcan_init"]["notes"]
    assert "cm4" not in m.parse_info(base)  # older firmware: no extra fields
    stats = struct.pack("<B", 1) + struct.pack("<IIIIBB", 10, 9, 1, 0, 0, 0) + bytes([8]) + bytes(32) + bytes([1]) + struct.pack("<II", 7, 2)
    assert "rx_nobus" not in m.parse_stats(stats)
    s2 = m.parse_stats(stats + struct.pack("<III", 11, 12, 13))
    assert (s2["rx_nobus"], s2["rx_ring_overrun"], s2["rx_ring_high"]) == (11, 12, 13) and s2["buses"][0]["tx_frames"] == 7
