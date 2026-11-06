"""Host tests for the head firmware fixes (Common/ + CM4 logic that is portable): flash slot log with power loss, HframeRx
resynchronisation, config Parse rejections and filter coverage, bit timing overflow, decode of short frames, stale logic,
receive ring, hproto behaviour (chunk retransmit, COMMIT flags, REBOOT magic, reply clamps), TX encode bit identity."""
import copy
import math
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

from canharness import blob as blobmod
from canharness import hwmap
from canharness.device.framing import encode_frame
from canharness.generators.head_c import head_tables
from canharness.model import Project

from test_head import build_sim, sample_project, tx_project

pytestmark = pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")

ROOT = Path(__file__).resolve().parents[1]
RANGE = 8  # HEAD_CFG_ERR_RANGE


def run(exe, *args, stdin=""):
    return subprocess.run([str(exe), *map(str, args)], input=stdin, capture_output=True, text=True)


# ---- plain C unit / simulation programs -------------------------------------------------------------------------------------

def test_unit_sim(tmp_path):
    exe = build_sim(tmp_path, "unit_sim", ["head_cfg.c", "head_decode.c", "head_encode.c", "head_rxq.c", "hframe.c", "hproto.c"], libs=["-lm"])
    r = run(exe)
    assert r.returncode == 0 and "0 failed" in r.stdout, r.stdout


def test_flash_slots_power_loss(tmp_path):
    exe = build_sim(tmp_path, "slots_sim", ["head_cfg.c", "head_slots.c"])
    r = run(exe)
    assert r.returncode == 0 and "0 failed" in r.stdout, r.stdout[-2000:]


# ---- HframeRx -------------------------------------------------------------------------------------------------------------

@pytest.fixture()
def frame_exe(tmp_path):
    return build_sim(tmp_path, "frame_sim", ["hframe.c"])


def feed(exe, cmds):
    r = run(exe, stdin="\n".join(cmds) + "\nx\n")
    assert r.returncode == 0
    lines = r.stdout.splitlines()
    return [l for l in lines if not l.startswith("cnt")], lines[-1]


def raw(msg_type, seq, payload=b""):
    return encode_frame(msg_type, seq, payload)


def test_hframe_basic_and_repeated_delimiters(frame_exe):
    out, cnt = feed(frame_exe, ["f 01 05 aabb", "b 00000000", "f 02 06 -", "b 0000"] + ["f 03 07 112233"])
    assert out == ["msg 0105aabb", "msg 0206", "msg 030711" "2233"]
    assert cnt == "cnt crc=0 ovf=0 desync=0"


def test_hframe_resync_after_garbage(frame_exe):
    good = raw(0x01, 9, b"\xaa\xbb")
    garbage = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])
    # 00 <garbage> 00 00 <valid frame> 00   (the valid frame with and without its own leading delimiter)
    for valid in (good, good[1:]):
        stream = b"\x00" + garbage + b"\x00\x00" + valid
        out, cnt = feed(frame_exe, [f"b {stream.hex()}"])
        assert out == ["msg 0109aabb"], valid.hex()
        assert cnt != "cnt crc=0 ovf=0 desync=0"  # the garbage block was counted, not silently swallowed


def test_hframe_resync_after_truncated_frame(frame_exe):
    good = raw(0x04, 3, b"\x01\x02\x03")
    truncated = good[:6]  # 00 + part of the COBS body, the closing delimiter never arrives
    for valid in (good, good[1:]):
        stream = truncated + b"\x00" + valid
        out, _ = feed(frame_exe, [f"b {stream.hex()}"])
        assert out == ["msg 0403010203"], valid.hex()
    # repeated: a long run of damaged frames must not poison the parser
    stream = b"".join(truncated + b"\x00" for _ in range(5)) + good
    out, _ = feed(frame_exe, [f"b {stream.hex()}"])
    assert out == ["msg 0403010203"]


def test_hframe_overflow_and_crc_counters(frame_exe):
    good = raw(0x01, 1, b"\x07")
    big = b"\x00" + b"\xff" * 400 + b"\x00"
    out, cnt = feed(frame_exe, [f"b {big.hex()}", f"b {good.hex()}"])
    assert out == ["msg 010107"] and "ovf=1" in cnt
    bad = bytearray(good)
    bad[3] ^= 0x40  # flip a body bit: CRC fails
    out, cnt = feed(frame_exe, [f"b {bytes(bad).hex()}", f"b {good.hex()}"])
    assert out == ["msg 010107"] and "crc=1" in cnt


def test_hframe_ascii_and_reset(frame_exe):
    out, _ = feed(frame_exe, ["f 01 01 -", "b " + b"ping\n".hex()])
    assert out == ["msg 0101", "line ping"]  # text still works after a good binary frame
    # a frame cut short by silence: Reset returns to idle so the next ASCII line is seen, and it is counted
    out, cnt = feed(frame_exe, ["b 00aabbcc", "r", "b " + b"hello\n".hex()])
    assert out == ["line hello"] and "desync=1" in cnt


# ---- timing -----------------------------------------------------------------------------------------------------------------

def test_timing_overflow_cases(tmp_path):
    exe = build_sim(tmp_path, "timing_sim", ["hcan_timing.c"])
    cases = [
        (80_000_000, 0x80000000 + 5_000_000, 800, 0),   # presc * bitrate wrapped to 10 MHz: used to "find" a timing
        (80_000_000, 1 << 23, 800, 0),                  # presc * bitrate == 0 (divide by zero)
        (80_000_000, 1 << 31, 800, 0),
        (80_000_000, 0xFFFFFFFF, 750, 1),
        (80_000_000, 0, 800, 0),
        (0, 500_000, 800, 0),
        (80_000_000, 80_000_000, 800, 0),               # < 4 time quanta per bit
        (80_000_000, 500_000, 1001, 0),                 # sample point above 100 %
        (80_000_000, 500_000, 800, 0),                  # sane control cases
        (80_000_000, 5_000_000, 750, 1),
    ]
    r = run(exe, stdin="\n".join(f"{c} {b} {s} {d}" for c, b, s, d in cases) + "\n")
    assert r.returncode == 0, r.stderr
    out = r.stdout.splitlines()
    assert out[:8] == ["none"] * 8
    assert out[8] != "none" and out[9] != "none"


# ---- Parse ------------------------------------------------------------------------------------------------------------------

def pack_tables(tables, config_hash=0x1234):
    body = bytearray()
    for sid, (name, fmt, size) in blobmod.SECTIONS.items():
        rows = tables[name]
        body += struct.pack("<HHI", sid, size, len(rows))
        for r in rows:
            body += struct.pack(fmt, *r)
    total = blobmod.HEADER_LEN + len(body)
    blob = bytearray(struct.pack("<IHHIIQII", blobmod.MAGIC, 1, blobmod.HEADER_LEN, total, len(blobmod.SECTIONS), config_hash, 0, 0) + body)
    blob[28:32] = struct.pack("<I", zlib.crc32(bytes(blob)))
    return bytes(blob)


@pytest.fixture()
def parser(tmp_path):
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])

    def parse(blob):
        f = tmp_path / "t.blob"
        f.write_bytes(blob)
        r = run(exe, f)
        return int(r.stdout.split()[1]), r.stdout.splitlines()[0]
    return parse


def base_tables():
    return {k: [list(r) for r in v] for k, v in head_tables(tx_project()).items()}


def mut(name, fn):
    t = base_tables()
    fn(t)
    return pack_tables(t)


def test_parse_accepts_generated_configs(parser, tmp_path):
    assert parser(blobmod.build(sample_project()))[0] == 0
    assert parser(blobmod.build(tx_project()))[0] == 0
    p = tx_project()
    for n in (1, 2, 0):  # widened hub filters (one per hub, one per type, ...) still cover every hub frame
        p.buses[0].ext_filters = n
        assert parser(blobmod.build(p))[0] == 0, n
    system = ROOT / "system.json"
    if system.exists():
        assert parser(blobmod.build(Project.load(system)))[0] == 0
    default = ROOT / "out" / "head" / "head_default.blob"
    if default.exists():
        assert parser(default.read_bytes())[0] == 0
    assert parser(pack_tables(base_tables()))[0] == 0


def _set(table, idx, field, value):
    return lambda t: t[table][idx].__setitem__(field, value)


REJECT = {
    # TX frame DLC rules (bus 1 is a classic bus, bus 0 the FD hub bus)
    "tx dlc 0": _set("tx_frames", 0, 2, 0),
    "tx dlc 9": _set("tx_frames", 0, 2, 9),
    "tx dlc 10": _set("tx_frames", 0, 2, 10),
    "tx dlc 65": _set("tx_frames", 0, 2, 65),
    "tx dlc 12 on a classic bus": _set("tx_frames", 0, 2, 12),
    "tx dlc 13 on an FD bus": lambda t: (t["tx_frames"][0].__setitem__(0, 0), t["tx_frames"][0].__setitem__(2, 13)),
    # buses
    "peripheral 0": _set("buses", 1, 2, 0),
    "peripheral 3": _set("buses", 1, 2, 3),
    "duplicate peripheral": _set("buses", 1, 2, 1),
    "bitrate 0": _set("buses", 1, 4, 0),
    "bitrate > 1M": _set("buses", 1, 4, 1_000_001),
    "fd without data rate": _set("buses", 0, 5, 0),
    "data rate > 8M": _set("buses", 0, 5, 8_000_001),
    "fd flag 2": _set("buses", 0, 1, 2),
    # filters
    "std filter id too big": lambda t: t["filters"].append([1, 0, 0, 0x800, 0x7FF]),
    "ext filter id too big": lambda t: t["filters"].append([1, 1, 0, 0x20000000, 0x1FFFFFFF]),
    "std filter mask too big": lambda t: t["filters"].append([1, 0, 0, 0x1, 0x800]),
    "filter bus out of range": lambda t: t["filters"].append([9, 0, 0, 0x1, 0x7FF]),
    "filter ext flag 2": lambda t: t["filters"].append([1, 2, 0, 0x1, 0x7FF]),
    "more than 64 ext filters on a bus": lambda t: t["filters"].extend([[1, 1, 0, i, 0x1FFFFFFF] for i in range(70)]),
    # hub routes and ids
    "route past min_len": lambda t: t["hub_routes"][0].__setitem__(1, t["hub_frames"][0][3]),
    "route length 5": _set("hub_routes", 0, 2, 5),
    "hub id_base outside id_mask": lambda t: t["hub_frames"][0].__setitem__(0, t["hub_frames"][0][0] | 1),
    "hub number 8": _set("hub_frames", 0, 2, 8),
    "hub min_len 65": _set("hub_frames", 0, 3, 65),
    "hub frames without a hub bus": _set("buses", 0, 0, 0),
    # filter coverage
    "no filter for the hub bus": lambda t: t.__setitem__("filters", [f for f in t["filters"] if f[0] != 0]),
    "no filter for the rx frame": lambda t: t.__setitem__("filters", [f for f in t["filters"] if not (f[0] == 1 and f[1] == 0)]),
    "no ext filter for the rx frame": lambda t: t.__setitem__("filters", [f for f in t["filters"] if not (f[0] == 1 and f[1] == 1)]),
    "hub filter id mismatch": lambda t: [f.__setitem__(3, f[3] ^ 0x00010000) for f in t["filters"] if f[0] == 0],
    "hub filter mask wider than the frame mask": lambda t: [f.__setitem__(4, 0x1FFFFFFF) for f in t["filters"] if f[0] == 0],
    # numbers and enums
    "rx NaN multiplier": _set("rx_signals", 0, 9, float("nan")),
    "rx Inf offset": _set("rx_signals", 0, 10, float("inf")),
    "tx NaN multiplier": _set("tx_signals", 0, 7, float("nan")),
    "tx Inf multiplier": _set("tx_signals", 0, 7, float("inf")),
    "tx zero multiplier": _set("tx_signals", 0, 7, 0.0),
    "tx -Inf offset": _set("tx_signals", 0, 8, float("-inf")),
    "channel value_type 3": _set("channels", 0, 1, 3),
    "channel source 5": _set("channels", 0, 2, 5),
    "rx can_id beyond 11 bits": _set("rx_frames", 0, 4, 0x800),
    "rx id_mask beyond 11 bits": _set("rx_frames", 0, 5, 0x1FFFFFFF),
    "rx can_id beyond 29 bits": _set("rx_frames", 1, 4, 0x20000000),
    "tx can_id beyond 11 bits": _set("tx_frames", 0, 4, 0x800),
    "tx can_id beyond 29 bits": _set("tx_frames", 1, 4, 0x20000000),
}


@pytest.mark.parametrize("name", sorted(REJECT))
def test_parse_rejects(parser, name):
    st, line = parser(mut(name, REJECT[name]))
    assert st == RANGE, (name, line)


def test_parse_accepts_boundaries(parser):
    def fd_dlc12(t):
        t["tx_frames"][0][0] = 0
        t["tx_frames"][0][2] = 12
    assert parser(mut("fd dlc 12", fd_dlc12))[0] == 0
    for dlc in (1, 8):
        assert parser(mut("classic", _set("tx_frames", 0, 2, dlc)))[0] == 0
    assert parser(mut("rates", lambda t: (t["buses"][1].__setitem__(4, 1_000_000), t["buses"][0].__setitem__(5, 8_000_000))))[0] == 0
    assert parser(mut("max std id", lambda t: t["tx_frames"][0].__setitem__(4, 0x7FF)))[0] == 0
    assert parser(mut("64 ext filters", lambda t: t["filters"].extend([[1, 1, 0, i << 8, 0x1FFFFF00] for i in range(63)])))[0] == 0  # + the one rx frame filter


# ---- decode through the real parser -----------------------------------------------------------------------------------------

def test_decode_short_frames_stale_and_duplicates(tmp_path):
    p = sample_project()
    tables = head_tables(p)
    chans = sorted((c for c in p.channels if c.enabled), key=lambda c: c.channel_id)
    slot = {c.channel_id: i for i, c in enumerate(chans)}
    blob_file = tmp_path / "cfg.blob"
    blob_file.write_bytes(blobmod.build(p))
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])

    def cell(line, cid):
        w, st = line.split()[1:][slot[cid]].split(":")
        return int(w, 16), int(st)

    # ecu1 (id 0x123, bus EXT1 = index 1): ch10 bits 0-15, ch11 bits 16-27, ch12 motorola bits 39..24 -> bytes 4-5
    short = "1 123 0 01000200 50"            # 4 bytes: ch10, ch11 present, ch12 needs bytes 4-5
    full = "1 123 0 0100020030004000 60"
    out = run(exe, blob_file, stdin="\n".join([short, "s 90", full, "s 130", "s 10000"]) + "\n").stdout.splitlines()[1:]
    assert out[0].split()[0] == "1" and cell(out[0], 10)[1] == 0 and cell(out[0], 11)[1] == 0
    assert cell(out[0], 12) == (0, 1)        # skipped: still never received, not "0 and fresh"
    assert cell(out[2], 12)[1] == 0          # present in the full frame
    assert cell(out[4], 10)[1] == 1          # timeout 200 ms passed
    # frame too short for any signal: dropped
    out = run(exe, blob_file, stdin="1 123 0 01 5\n").stdout.splitlines()[1:]
    assert out[0].split()[0] == "0"
    # a frame stamped newer than the staleness clock is fresh, not a 49-day-old one
    out = run(exe, blob_file, stdin="\n".join(["1 123 0 0100020030004000 1000", "s 999"]) + "\n").stdout.splitlines()[1:]
    assert cell(out[1], 10)[1] == 0
    # hub frame counter: same counter twice is a duplicate, not 255 lost
    fr = next(f for f in __import__("canharness.packing", fromlist=["pack"]).pack(p)[0] if f.rate_hz == 200)
    data = bytes(16).hex()
    ids = [fr.id_base | (c << 8) for c in (10, 10, 11, 14)]
    out = run(exe, blob_file, stdin="\n".join(f"0 {i:x} 1 {data} {t}" for t, i in enumerate(ids)) + "\n").stdout.splitlines()[1:]
    assert [l.rsplit("lost=", 1)[1] for l in out] == ["0", "0", "0", "2"]


def test_decode_timeout_zero_means_never_stale(tmp_path):
    p = sample_project()
    t = {k: [list(r) for r in v] for k, v in head_tables(p).items()}
    for ch in t["channels"]:
        ch[5] = 0
    f = tmp_path / "cfg.blob"
    f.write_bytes(pack_tables(t))
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])
    out = run(exe, f, stdin="\n".join(["1 123 0 0100020030004000 5", "s 100000000"]) + "\n").stdout.splitlines()[1:]
    cells = out[1].split()[1:-1]
    assert sum(c.endswith(":0") for c in cells) == 3  # ch10, ch11, ch12 were received and stay valid forever; all others never were


# ---- TX encode against the Python reference, extra cases -------------------------------------------------------------------

def test_tx_encode_exact_u32(tmp_path):
    from canharness import rxdecode
    p = tx_project()
    blob_file = tmp_path / "cfg.blob"
    blob_file.write_bytes(blobmod.build(p))
    exe = build_sim(tmp_path, "encode_sim", ["head_cfg.c", "head_decode.c", "head_encode.c"])
    chans = sorted((c for c in p.channels if c.enabled), key=lambda c: c.channel_id)
    bits = [0] * len(chans)
    slot = {c.channel_id: i for i, c in enumerate(chans)}
    bits[slot[3]] = 0xFFFFFF01  # ch3 -> motec2 signal: u32, multiplier 1, 32 bits big endian at bit 39: exact, not quantised
    r = run(exe, blob_file, stdin="v " + " ".join(f"{b:x}" for b in bits) + "\n")
    frames = r.stdout.splitlines()[1:3]
    # signal start bit 39 Motorola 32 bits: bytes 4..7 hold FF FF FF 01
    assert frames[1][8:] == "ffffff01"


# ---- INFO / STATS encoding (hproto) ------------------------------------------------------------------------------------------

def proto_request(exe, msg_type, payload=b"", env=None):
    import os
    from canharness.device.framing import FrameParser
    r = subprocess.run([str(exe)], input=encode_frame(msg_type, 1, payload), capture_output=True, env={**os.environ, **(env or {})})
    assert r.returncode == 0
    got = []
    fp = FrameParser()
    for b in r.stdout:
        got.extend(fp.feed(bytes([b])))
    return got


@pytest.fixture()
def proto_exe(tmp_path):
    return build_sim(tmp_path, "proto_sim", ["head_cfg.c", "head_decode.c", "head_slots.c", "hframe.c", "hproto.c"])


def _reply_payload(frames):
    msgs = [f[1] for f in frames if f[0] == "frame" and f[1][0] != 0xB1]
    assert len(msgs) == 1, frames
    return msgs[0][2]


def test_info_appended_fields(proto_exe):
    """INFO keeps its first 48 bytes and appends 32 bytes (80 in total): cm4 status, boot apply, UART/framing counters, tcan status."""
    frames = proto_request(proto_exe, 0x01)
    pl = _reply_payload(frames)
    assert len(pl) == 80
    assert pl[0] == 1 and struct.unpack_from("<H", pl, 2)[0] == 0x0102 and struct.unpack_from("<I", pl, 4)[0] == 1234  # old layout unchanged
    assert struct.unpack_from("<I", pl, 44)[0] == 80000000
    assert pl[48] == 0x02 and pl[49] == 2 and struct.unpack_from("<H", pl, 50)[0] == 300
    assert struct.unpack_from("<6I", pl, 52) == (11, 12, 13, 14, 15, 16)
    assert pl[76] == 0x24 and pl[77:80] == b"\0\0\0"


def test_stats_appended_rx_counters(proto_exe):
    frames = proto_request(proto_exe, 0x11, env={"HPROTO_SIM_STATS_FULL": "1"})
    pl = _reply_payload(frames)
    nb = 2
    assert len(pl) == 1 + 18 * nb + 1 + 32 + 1 + 8 * nb + 12
    assert pl[0] == nb
    assert struct.unpack_from("<4I", pl, 1 + 18) == (101, 201, 301, 401) and pl[1 + 18 + 16] == 1 and pl[1 + 18 + 17] == 0
    o = 1 + 18 * nb
    assert pl[o] == 8 and struct.unpack_from("<I", pl, o + 1 + 4 * 7)[0] == 707
    o += 1 + 32
    assert pl[o] == nb and struct.unpack_from("<2I", pl, o + 1 + 8) == (501, 601)
    o += 1 + 8 * nb
    assert struct.unpack_from("<3I", pl, o) == (21, 22, 23)  # appended last: rx_nobus, rx_ring_overrun, rx_ring_high


# ---- CM7 console helpers --------------------------------------------------------------------------------------------------

@pytest.fixture()
def text_exe(tmp_path):
    cm7 = ROOT.parent / "CM7" / "Core" / "Inc"
    exe = tmp_path / "text_sim"
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{cm7}", "-o", str(exe), str(Path(__file__).parent / "c" / "text_sim.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return exe


def test_console_text_filter_and_hex(text_exe):
    lines = ["p ping", "p send:tcan:0F5A", r"p ping\x01", r"p \xff\xfe", r"p a\x7fb", r"p tab\x09x",
             "h 0F5A", "h 0f5a", "h 0F5", "h 0F5A0", "h 0F5G", "h ", r"h 0F\x005A", "h  F5A",
             "w 0F", "w 10", "w 50", "w 51", "w 5C", "w 0C", "w 13", "w 14", "w 15", "w 4E", "w 56", "w 00"]
    out = run(text_exe, stdin="\n".join(lines) + "\n").stdout.splitlines()
    assert out == ["p 1", "p 1", "p 0", "p 0", "p 0", "p 0",
                   "h 1 0f 5a", "h 1 0f 5a", "h 0", "h 0", "h 0", "h 0", "h 0", "h 0",
                   "w 1", "w 1", "w 1", "w 1", "w 1", "w 0", "w 0", "w 0", "w 0", "w 0", "w 0", "w 0"]


# ---- AFE volts -----------------------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("vref", [5.0, 3.3])
def test_analog_hub_channel_is_float_base_unit_and_no_data_code_goes_stale(tmp_path, vref):
    """Analog hub channels are scaled to float32 volts in the head (code / 2^23 * Vref); the hub's "no sample" code -8388608 is not stored."""
    from canharness.packing import pack
    p = sample_project()
    p.settings.afe_vref_v = vref
    (tmp_path / "cfg.blob").write_bytes(blobmod.build(p))
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])
    fr = next(f for f in pack(p)[0] if f.rate_hz == 200)
    pl = next(x for x in fr.placements if x.channel.channel_id == 1)
    chans = sorted((c for c in p.channels if c.enabled), key=lambda c: c.channel_id)
    slot = [c.channel_id for c in chans].index(1)

    def frame(code, ts):
        data = bytearray(16)
        data[pl.offset:pl.offset + 3] = (code & 0xFFFFFF).to_bytes(3, "little")
        return f"0 {fr.id_base:x} 1 {bytes(data).hex()} {ts}"

    lines = [frame(0x400000, 0), frame(-0x200000, 1), frame(-8388608, 2), frame(8388607, 100)]
    out = subprocess.run([str(exe), str(tmp_path / "cfg.blob")], input="\n".join(lines) + "\n", capture_output=True, text=True).stdout.splitlines()[1:]
    cell = lambda i: out[i].split()[1 + slot]
    f = lambda i: struct.unpack("<f", int(cell(i).split(":")[0], 16).to_bytes(4, "little"))[0]
    unit = 1.0 / hwmap.excitation_current_a(p.channels[0].sensor_type.value) if p.channels[0].sensor_type.value == "resistive" else 1.0  # ohms per volt
    assert f(0) == pytest.approx(0.5 * vref * unit, rel=1e-6)    # half scale
    assert f(1) == pytest.approx(-0.25 * vref * unit, rel=1e-6)  # negative code, sign extended from 3 bytes
    assert f(2) == f(1) and cell(2).endswith(":0")               # "no sample": value and freshness unchanged here ...
    assert f(3) == pytest.approx(8388607 / 2**23 * vref * unit, rel=1e-6)


def _blob_with_route(mutator):
    b = bytearray(blobmod.build(sample_project()))
    off, nsec = 32, struct.unpack_from("<I", b, 12)[0]
    for _ in range(nsec):
        sid, esize, count = struct.unpack_from("<HHI", b, off)
        if sid == 3:
            mutator(b, off + 8, esize)
        off += 8 + esize * count
    b[28:32] = b"\0\0\0\0"
    b[28:32] = struct.pack("<I", zlib.crc32(bytes(b)))
    return bytes(b)


def _route(b, o, e, pred):
    """offset of the first route element (elements start at o, size e) for which pred(flags, length) holds"""
    for i in range(len(b)):
        q = o + i * e
        if b[q + 4] is None or q + e > len(b):
            break
        if pred(b[q + 5], b[q + 3]):
            return q
    raise AssertionError("no such route")


@pytest.mark.parametrize("why,mut", [
    ("unknown flag", lambda b, o, e: b.__setitem__(_route(b, o, e, lambda f, n: True) + 5, 0x10)),
    ("scaled but integer channel", lambda b, o, e: b.__setitem__(_route(b, o, e, lambda f, n: f == 0) + 5, 1)),
    ("NaN multiplier", lambda b, o, e: b.__setitem__(slice(_route(b, o, e, lambda f, n: f == 3) + 8, _route(b, o, e, lambda f, n: f == 3) + 12), struct.pack("<I", 0x7FC00000))),
    ("no-data flag on a 1 byte field", lambda b, o, e: b.__setitem__(_route(b, o, e, lambda f, n: f == 0 and n == 1) + 5, 2)),
])
def test_route_flags_rejected(tmp_path, why, mut):
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])
    (tmp_path / "bad.blob").write_bytes(_blob_with_route(mut))
    out = subprocess.run([str(exe), str(tmp_path / "bad.blob")], input="", capture_output=True, text=True).stdout.splitlines()
    assert out[0].startswith("cfg ") and not out[0].startswith("cfg 0 ok"), why
