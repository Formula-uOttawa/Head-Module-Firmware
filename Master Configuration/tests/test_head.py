import math
import random
import shutil
import struct
import subprocess

import pytest

from canharness import hwmap, rxdecode
from canharness.generators.head_c import generate_head, plan_filters
from canharness.model import Bus, Channel, Hub, Project, RxFrame, RxSignal, SensorType
from canharness.validate import errors, validate


def hubch(i, pin, **kw):
    d = dict(channel_id=i, name=f"H{i}", short_name=f"H{i}", hub=0, pin_sig_pos=pin, sample_rate_hz=200)
    d.update(kw)
    return Channel(**d)


def canch(i):
    return Channel(channel_id=i, name=f"C{i}", short_name=f"C{i}", source="can_rx", sensor_type=SensorType.CAN, sample_rate_hz=100)


def sig(cid, start, n, **kw):
    return RxSignal(channel_id=cid, start_bit=start, bit_length=n, **kw)


def sample_project():
    # analog channels carry the raw 24-bit signed code (3 bytes); the digital ones keep 1-, 2- and 4-byte (unsigned) decode covered
    chans = [hubch(1, 1, raw_bytes=3, signed=True), hubch(2, 21, sensor_type=SensorType.DIGITAL, signed=True, raw_bytes=1),
             hubch(3, 23, sensor_type=SensorType.DIGITAL, raw_bytes=4), hubch(4, 22, sensor_type=SensorType.DIGITAL, raw_bytes=2, sample_rate_hz=10)]
    chans += [canch(i) for i in range(10, 18)]
    f1 = RxFrame(name="ecu1", bus="EXT1", can_id=0x123, dlc=8, timeout_ms=200, signals=[
        sig(10, 0, 16, multiplier=0.1, offset=-40.0),
        sig(11, 16, 12, signed=True),
        sig(12, 39, 16, byte_order="motorola", signed=True, multiplier=0.5),
    ])
    f2 = RxFrame(name="ecu2", bus="EXT1", can_id=0x18FF1234, extended=True, dlc=8, mux_start_bit=0, mux_bit_length=8, signals=[
        sig(13, 8, 16, mux_value=0), sig(14, 8, 16, mux_value=1, multiplier=2.0),
        sig(15, 32, 32, is_float=True, multiplier=1.5, offset=1.0), sig(16, 31, 8, byte_order="motorola"),
    ])
    return Project(hubs=[Hub(hub_id=0, name="h")], channels=chans, rx_frames=[f1, f2])


def test_motorola_matches_dbc_convention():
    assert rxdecode.extract_bits(b"\x12\x34", 7, 16, "motorola") == 0x1234
    assert rxdecode.extract_bits(b"\x00\x00\x00\x00\xAB\xCD\x00\x00", 39, 16, "motorola") == 0xABCD
    assert rxdecode.extract_bits(b"\x34\x12", 0, 16, "intel") == 0x1234


def test_sample_project_valid():
    assert not errors(validate(sample_project()))


def test_filter_plan_and_widening():
    p = sample_project()
    plan = plan_filters(p)
    assert plan["EXT1"]["std"] == [(0x123, 0x7FF)] and plan["EXT1"]["ext"] == [(0x18FF1234, 0x1FFFFFFF)]
    assert plan["HUB"]["ext_needed"] == 2 and all(m == 0x1FFF0000 for _, m in plan["HUB"]["ext"])
    p.buses[0].ext_filters = 1  # fewer filters than hub frames -> widened to one per hub
    assert plan_filters(p)["HUB"]["ext"] == [(0x00000000, 0x1F000000)]


def test_rx_validation_rules():
    p = sample_project()
    p.rx_frames[0].signals += [sig(10, 4, 8), sig(99, 0, 8), sig(17, 60, 8), sig(16, 0, 33)]  # dup channel, missing, past dlc, too long
    p.rx_frames.append(RxFrame(name="dup", bus="EXT1", can_id=0x123))
    p.rx_frames.append(RxFrame(name="hubbus", bus="HUB", can_id=0x1))
    p.rx_frames.append(RxFrame(name="nobus", bus="NOPE", can_id=0x1))
    p.rx_frames.append(RxFrame(name="big", bus="EXT1", can_id=0x800, dlc=9))
    msgs = " | ".join(i.message for i in errors(validate(p)))
    for frag in ["already decoded", "does not exist", "beyond", "bit_length", "duplicate CAN ID", "hub bus", "not defined", "out of range", "DLC 9"]:
        assert frag in msgs, frag


def test_overlap_and_mux_exclusivity():
    p = sample_project()
    p.rx_frames[1].signals.append(sig(17, 12, 8))  # overlaps muxed signals' bits and is unmuxed
    assert any("overlaps" in i.message for i in errors(validate(p)))
    p = sample_project()  # ch13/ch14 share bits but have different mux values -> fine
    assert not any("overlaps" in i.message for i in errors(validate(p)))


def test_filter_capacity_error():
    p = sample_project()
    p.buses[1].std_filters = 0
    assert any("standard filters" in i.message for i in errors(validate(p)))


def test_python_decode_mux_and_float():
    f = sample_project().rx_frames[1]
    data = bytes([1, 0x34, 0x12, 0, *struct.pack("<f", 2.5)])
    out = rxdecode.decode_frame(f, data)
    assert 13 not in out and out[14] == 0x1234 * 2.0 and out[15] == pytest.approx(2.5 * 1.5 + 1.0)


def build_sim(tmp_path, harness, sources, libs=()):
    """Compile a host test harness against the real firmware sources in ../Common."""
    from pathlib import Path
    common = Path(__file__).resolve().parents[2] / "Common"
    exe = tmp_path / harness
    hub = common.parent.parent / "SensorHubV1" / "Core"  # hub sources are named hub_*.c; everything else is shared Common code
    srcs = [str((hub if f.startswith("hub_") else common) / "Src" / f) for f in sources]
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", f"-I{common / 'Inc'}", f"-I{hub / 'Inc'}", "-o", str(exe),
           str(Path(__file__).parent / "c" / f"{harness}.c"), *srcs, *libs]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return exe


@pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")
def test_firmware_decode_matches_reference(tmp_path):
    from canharness import blob as blobmod
    from canharness.packing import pack
    p = sample_project()
    blob_file = tmp_path / "cfg.blob"
    blob_file.write_bytes(blobmod.build(p))
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])

    rng = random.Random(7)
    chans = sorted((c for c in p.channels if c.enabled), key=lambda c: c.channel_id)
    slot = {c.channel_id: i for i, c in enumerate(chans)}
    frames, _ = pack(p)
    fr200 = next(f for f in frames if f.rate_hz == 200)
    vals = {}  # slot -> (kind, value)
    lines, expected = [], []
    for n in range(300):
        pick = rng.randrange(3)
        if pick == 0:
            fid = fr200.id_base | (rng.randrange(256) << 8)  # frame counter bits must be ignored
            data = bytes(rng.randrange(256) for _ in range(16))
            lines.append(f"0 {fid:x} 1 {data.hex()} {n}")
            for pl in fr200.placements:
                raw = int.from_bytes(data[pl.offset:pl.offset + pl.length], "little")
                if pl.channel.signed and raw & (1 << (8 * pl.length - 1)):
                    raw -= 1 << (8 * pl.length)
                if pl.channel.sensor_type != SensorType.DIGITAL:  # analog hub channel: float volts, code -8388608 = "no sample" (not stored)
                    if raw != -8388608:
                        vals[slot[pl.channel.channel_id]] = ("f", raw * hwmap.volts_per_count(pl.channel.sensor_type.value, p.settings.afe_vref_v))
                else:
                    vals[slot[pl.channel.channel_id]] = ("i" if pl.channel.signed else "u", raw)
        else:
            f = p.rx_frames[pick - 1]
            data = bytes(rng.randrange(256) for _ in range(8))
            if f.mux_start_bit is not None:
                data = bytes([rng.randrange(3)]) + data[1:]
                if rng.random() < .5:
                    data = data[:4] + struct.pack("<f", rng.uniform(-1000, 1000))
            lines.append(f"1 {f.can_id:x} {int(f.extended)} {data.hex()} {n}")
            for cid, v in rxdecode.decode_frame(f, data).items():
                vals[slot[cid]] = ("f", v)
        expected.append(dict(vals))
    out = subprocess.run([str(exe), str(blob_file)], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True).stdout.splitlines()
    assert out[0].startswith("cfg 0 ok")
    out = out[1:]
    assert len(out) == len(lines)
    for line, exp in zip(out, expected):
        cells = line.split()[1:]
        for s_, (kind, v) in exp.items():
            word = int(cells[s_].split(":")[0], 16)
            if kind == "f":
                got = struct.unpack("<f", word.to_bytes(4, "little"))[0]
                assert got == pytest.approx(v, rel=1e-5, abs=1e-4) or (math.isnan(got) and math.isnan(v))
            else:
                got = word - (1 << 32) if kind == "i" and word >= 1 << 31 else word
                assert got == v


@pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")
def test_firmware_staleness_and_hub_loss(tmp_path):
    from canharness import blob as blobmod
    from canharness.packing import pack
    p = sample_project()
    (tmp_path / "cfg.blob").write_bytes(blobmod.build(p))
    exe = build_sim(tmp_path, "decode_sim", ["head_cfg.c", "head_decode.c"])
    fr200 = next(f for f in pack(p)[0] if f.rate_hz == 200)
    ids = [fr200.id_base | (c << 8) for c in (10, 11, 14)]  # counter 10, 11, then 14 -> 2 frames lost
    data = bytes(16).hex()
    lines = [f"0 {i:x} 1 {data} {t}" for i, t in zip(ids, (0, 1, 2))] + [f"0 {ids[0]:x} 1 {data} 5000"]
    out = subprocess.run([str(exe), str(tmp_path / "cfg.blob")], input="\n".join(lines) + "\n", capture_output=True, text=True).stdout.splitlines()[1:]
    assert out[1].endswith("lost=0") and out[2].endswith("lost=2")
    first = out[0].split()[1]
    assert first.endswith(":0")                      # just received
    assert out[3].split()[1].endswith(":0")          # re-received at 5000
    assert out[3].split()[-2].endswith(":1")         # a never-received CAN channel stays stale


# ---- bit timing ------------------------------------------------------------------------------------------------------

def test_bit_timing_known_values():
    from canharness.cantiming import compute_timing, achievable, nearby
    assert compute_timing(51_200_000, 320_000, 800, False) == (1, 128, 31, 31) or compute_timing(51_200_000, 320_000, 800, False)[0] == 1
    assert achievable(51_200_000, 3_200_000, True) and not achievable(88_000_000, 3_200_000, True)
    assert 1_600_000 in nearby(88_000_000, 3_200_000, True)


@pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")
def test_c_timing_matches_python(tmp_path):
    from canharness.cantiming import compute_timing
    exe = build_sim(tmp_path, "timing_sim", ["hcan_timing.c"])
    rng = random.Random(3)
    cases = [(clk, rate, sp, d) for clk in (51_200_000, 80_000_000, 88_000_000, 40_000_000)
             for rate in (125_000, 250_000, 320_000, 500_000, 800_000, 1_000_000, 1_600_000, 2_000_000, 3_200_000, 4_000_000, 5_000_000, 8_000_000)
             for sp, d in ((800, 0), (750, 1))]
    cases += [(rng.choice((51_200_000, 88_000_000)), rng.randrange(100_000, 9_000_000), rng.choice((700, 800, 875)), rng.randrange(2)) for _ in range(150)]
    out = subprocess.run([str(exe)], input="\n".join(f"{c} {r} {s} {d}" for c, r, s, d in cases) + "\n", capture_output=True, text=True, check=True).stdout.splitlines()
    for (clk, rate, sp, d), line in zip(cases, out):
        py = compute_timing(clk, rate, sp, bool(d))
        assert line == ("none" if py is None else " ".join(map(str, py))), (clk, rate, sp, d)


def test_bus_rate_validation_reports_clock_problem():
    p = sample_project()
    assert not errors(validate(p))                     # defaults: 80 MHz on both boards, 320k / 5M hub bus, 500k external
    p.settings.head_fdcan_kernel_hz, p.settings.hub_fdcan_kernel_hz = 88_000_000, 51_200_000   # the boards as they were before
    p.buses[0].data_bitrate = 3_200_000
    msgs = [i.message for i in errors(validate(p))]
    assert any("data rate 3.2 Mbit/s cannot be generated exactly by the head" in m and "1.6 Mbit/s" in m for m in msgs)
    p.settings.head_fdcan_kernel_hz = 51_200_000   # if the head's clock matched the hub's, 3.2 Mbit/s would work...
    msgs = [i.message for i in errors(validate(p))]
    assert not any("data rate" in m for m in msgs)
    assert any("500 kbit/s" in m for m in msgs)    # ...but the external bus's 500 kbit/s no longer divides 51.2 MHz
    p.buses[1].bitrate = 800_000
    assert not errors(validate(p))


# ---- transmit frames (Motec) -----------------------------------------------------------------------------------------

def tx_project():
    from canharness.model import TxFrame, TxSignal
    p = sample_project()
    p.tx_frames = [
        TxFrame(name="motec1", bus="EXT1", can_id=0x200, dlc=8, rate_hz=100, signals=[
            TxSignal(channel_id=1, start_bit=7, bit_length=16),                                    # u32 hub value, big endian
            TxSignal(channel_id=2, start_bit=23, bit_length=8, signed=True, multiplier=2.0),       # i32 hub value, scaled
            TxSignal(channel_id=10, start_bit=32, bit_length=16, byte_order="intel", multiplier=0.1, offset=-40.0),  # float ch
            TxSignal(channel_id=12, start_bit=51, bit_length=12, signed=True, multiplier=0.5),
        ]),
        TxFrame(name="motec2", bus="EXT1", can_id=0x18FFAA01, extended=True, dlc=8, rate_hz=10, signals=[
            TxSignal(channel_id=11, start_bit=0, bit_length=32, byte_order="intel", is_float=True, multiplier=1.5, offset=1.0),
            TxSignal(channel_id=3, start_bit=39, bit_length=32),
        ]),
    ]
    return p


def test_tx_validation():
    from canharness.model import TxFrame, TxSignal
    assert not errors(validate(tx_project()))
    p = tx_project()
    p.tx_frames[0].signals += [TxSignal(channel_id=1, start_bit=7, bit_length=8), TxSignal(channel_id=99, start_bit=0, bit_length=8),
                               TxSignal(channel_id=4, start_bit=100, bit_length=8), TxSignal(channel_id=4, start_bit=0, bit_length=40),
                               TxSignal(channel_id=4, start_bit=0, bit_length=8, multiplier=0)]
    p.tx_frames += [TxFrame(name="dup", bus="EXT1", can_id=0x200), TxFrame(name="hub", bus="HUB", can_id=0x1),
                    TxFrame(name="nobus", bus="X", can_id=1), TxFrame(name="big", bus="EXT1", can_id=0x800, dlc=9, rate_hz=2000)]
    msgs = " | ".join(i.message for i in errors(validate(p)))
    for frag in ["overlaps", "does not exist", "beyond", "bit_length", "multiplier", "duplicate CAN ID", "hub bus", "not defined", "out of range", "DLC 9", "rate must"]:
        assert frag in msgs, frag


def test_tx_encode_reference_roundtrip():
    f = tx_project().tx_frames[0]
    data = rxdecode.encode_tx_frame(f, {1: 0x1234, 2: -5, 10: 25.0, 12: -3.0})
    assert data[0:2] == b"\x12\x34" and data[2] == 0xFD          # -5 / 2.0 = -2.5 -> -3 (half away from zero) = 0xFD
    assert rxdecode.extract_bits(data, 32, 16, "intel") == 650    # (25 + 40) / 0.1
    # Motorola frame values survive a decode with the receiver's formula
    assert rxdecode.extract_bits(data, 7, 16, "motorola") == 0x1234


@pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")
def test_firmware_tx_matches_reference(tmp_path):
    from canharness import blob as blobmod
    p = tx_project()
    blob_file = tmp_path / "cfg.blob"
    blob_file.write_bytes(blobmod.build(p))
    exe = build_sim(tmp_path, "encode_sim", ["head_cfg.c", "head_decode.c", "head_encode.c"])
    chans = sorted((c for c in p.channels if c.enabled), key=lambda c: c.channel_id)
    rng = random.Random(11)
    lines, expected = [], []
    for _ in range(200):
        bits, vals = [], {}
        for c in chans:
            if c.source == "can_rx":
                v = rng.choice([0.0, rng.uniform(-500, 500), rng.uniform(-1e6, 1e6)])
                bits.append(struct.unpack("<I", struct.pack("<f", v))[0]); vals[c.channel_id] = struct.unpack("<f", struct.pack("<f", v))[0]
            elif c.source == "hub" and c.sensor_type != SensorType.DIGITAL:  # analog hub channel: float volts
                v = struct.unpack("<f", struct.pack("<f", rng.uniform(-5, 5)))[0]
                bits.append(struct.unpack("<I", struct.pack("<f", v))[0]); vals[c.channel_id] = v
            elif c.signed:
                v = rng.randrange(-200, 200) if rng.random() < .8 else rng.randrange(-2**31, 2**31)
                bits.append(v & 0xFFFFFFFF); vals[c.channel_id] = float(v)
            else:
                v = rng.randrange(0, 70000) if rng.random() < .8 else rng.randrange(0, 2**24)
                bits.append(v); vals[c.channel_id] = float(v)
        lines.append("v " + " ".join(f"{b:x}" for b in bits))
        expected += [rxdecode.encode_tx_frame(f, vals).hex() for f in p.tx_frames]
    r = subprocess.run([str(exe), str(tmp_path / "cfg.blob")], input="\n".join(lines) + "\n", capture_output=True, text=True)
    out = r.stdout.splitlines()
    assert out[0].startswith("cfg 0 ok tf=2")
    assert out[1:1 + len(expected)] == expected


@pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")
def test_firmware_tx_schedule(tmp_path):
    from canharness import blob as blobmod
    (tmp_path / "cfg.blob").write_bytes(blobmod.build(tx_project()))
    exe = build_sim(tmp_path, "encode_sim", ["head_cfg.c", "head_decode.c", "head_encode.c"])
    # motec1 every 10 ms, motec2 every 100 ms. The frames are staggered by index (motec2 starts 1 ms after motec1), a busy bus keeps
    # the phase and retries every pass until the next period would start (only then the slot is skipped and counted), and a
    # 1 s stall must not burst
    script = ["t 0", "t 5", "t 10", "b 1", "t 20", "b 0", "t 21", "t 100", "t 1100", "t 1101",
              "b 1", "t 1110", "t 1119", "t 1120", "b 0", "t 1130"]
    r = subprocess.run([str(exe), str(tmp_path / "cfg.blob")], input="\n".join(script) + "\n", capture_output=True, text=True)
    out = r.stdout.splitlines()
    sent = [l.count("[") for l in out[1:-1]]
    # t=0 motec1; 5 motec2 (due at 1); 10 motec1; 20 bus busy; 21 retried; 100 motec1 (stall resync); 1100 both; 1101 none;
    # 1110..1119 busy (retrying, nothing dropped yet); 1120 one full period late -> skipped; 1130 sent again
    assert sent == [1, 1, 1, 0, 1, 1, 2, 0, 0, 0, 0, 1]
    assert out[-1].endswith("dropped=1")
