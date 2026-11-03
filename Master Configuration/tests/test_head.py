import math
import random
import shutil
import struct
import subprocess

import pytest

from canharness import rxdecode
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
    chans = [hubch(1, 1), hubch(2, 2, signed=True, raw_bytes=1), hubch(3, 3, raw_bytes=4), hubch(4, 4, sample_rate_hz=10)]
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


@pytest.mark.skipif(not shutil.which("cc"), reason="no host C compiler")
def test_generated_c_matches_reference(tmp_path):
    p = sample_project()
    files = generate_head(p)
    for n, t in files.items():
        (tmp_path / n).write_text(t)
    (tmp_path / "main.c").write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "head_config.h"
int main(void) {
    char line[512]; Head_Init();
    while (fgets(line, sizeof line, stdin)) {
        unsigned bus, ext; unsigned long id; char hexd[200]; unsigned long now;
        if (sscanf(line, "%u %lx %u %199s %lu", &bus, &id, &ext, hexd, &now) != 5) break;
        uint8_t d[64]; size_t n = strlen(hexd) / 2;
        for (size_t i = 0; i < n; i++) { unsigned b; sscanf(hexd + 2 * i, "%2x", &b); d[i] = (uint8_t)b; }
        int h = Head_ProcessFrame(bus, id, ext, d, (uint8_t)n, now);
        printf("%d", h);
        for (int s = 0; s < HEAD_CHANNEL_COUNT; s++) printf(" %08x:%d", g_head_values[s].u, Head_ChannelIsStale(s, now));
        printf("\n");
    }
    return 0;
}''')
    exe = tmp_path / "t"
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-o", str(exe), *map(str, tmp_path.glob("*.c"))], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    rng = random.Random(7)
    chans = sorted((c for c in p.channels if c.enabled), key=lambda c: c.channel_id)
    slot = {c.channel_id: i for i, c in enumerate(chans)}
    hub_ids = [0x00200000, 0x00800000]  # 200 Hz idx0, 10 Hz idx0 (+ counter bits must be ignored)
    vals = {}  # slot -> ("u"|"i"|"f", value)
    lines, expected = [], []
    for n in range(300):
        pick = rng.randrange(3)
        if pick == 0:
            fid = hub_ids[0] | (rng.randrange(256) << 8)
            data = bytes(rng.randrange(256) for _ in range(16))
            lines.append(f"0 {fid:x} 1 {data.hex()} {n}")
            # channels 1 (u16 @4), 2 (i8 @6), 3 (u32 @8)  -- offsets come from the packer
            from canharness.packing import pack
            frames, _ = pack(p)
            fr = next(f for f in frames if f.rate_hz == 200)
            for pl in fr.placements:
                raw = int.from_bytes(data[pl.offset:pl.offset + pl.length], "little")
                if pl.channel.signed and raw & (1 << (8 * pl.length - 1)):
                    raw -= 1 << (8 * pl.length)
                vals[slot[pl.channel.channel_id]] = ("i" if pl.channel.signed else "u", raw)
        else:
            f = p.rx_frames[pick - 1]
            data = bytes(rng.randrange(256) for _ in range(8))
            if f.mux_start_bit is not None:
                data = bytes([rng.randrange(3)]) + data[1:]
                if rng.random() < .5:
                    data = data[:4] + struct.pack("<f", rng.uniform(-1000, 1000))
            lines.append(f"{1} {f.can_id:x} {int(f.extended)} {data.hex()} {n}")
            for cid, v in rxdecode.decode_frame(f, data).items():
                vals[slot[cid]] = ("f", v)
        expected.append(dict(vals))
    out = subprocess.run([str(exe)], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True).stdout.splitlines()
    assert len(out) == len(lines)
    for line, exp in zip(out, expected):
        cells = line.split()[1:]
        for s, (kind, v) in exp.items():
            word = int(cells[s].split(":")[0], 16)
            if kind == "f":
                got = struct.unpack("<f", word.to_bytes(4, "little"))[0]
                assert got == pytest.approx(v, rel=1e-5, abs=1e-4) or (math.isnan(got) and math.isnan(v))
            else:
                got = word - (1 << 32) if kind == "i" and word >= 1 << 31 else word
                assert got == v
