import re
from pathlib import Path

import pytest

from canharness import canid
from canharness.generators.dash_json import generate_dash
from canharness.generators.hub_c import generate_hub
from canharness.legacy import import_yaml
from canharness.model import Channel, Hub, Project, SensorType
from canharness.packing import pack
from canharness.validate import errors, validate

HUB_DIR = Path(__file__).resolve().parents[3] / "SensorHubV1"
LEGACY_YAML = HUB_DIR / "config" / "rear_sensor_config.yaml"
LEGACY_C = HUB_DIR / "Core" / "Src" / "sensor_config_generated.c"


def ch(i, pin, **kw):
    d = dict(channel_id=i, name=f"S{i}", short_name=f"S{i}", hub=0, pin_sig_pos=pin, sample_rate_hz=200)
    d.update(kw)
    return Channel(**d)


def proj(*chs):
    return Project(hubs=[Hub(hub_id=0, name="h")], channels=list(chs))


def test_id_matches_spreadsheet():
    # Rear hub (hub 0) rows from the xlsx: 0b0000000100000 / 0b0000001000000 / 0b0000010000000 / ...1
    assert canid.id_base(0, 0, 200, 0) == 0b0000000100000 << 16
    assert canid.id_base(0, 0, 100, 0) == 0b0000001000000 << 16
    assert canid.id_base(0, 0, 10, 1) == 0b0000010000001 << 16
    # Front hub (hub 1) 100 Hz
    assert canid.id_base(0, 1, 100, 0) == 0b0000101000000 << 16
    assert canid.decode(canid.id_base(0, 2, 200, 3) | (7 << 8)) == {
        "type": 0, "hub": 2, "rate_hz": 200, "frame_idx": 3, "counter": 7, "other": 0}


@pytest.mark.parametrize("args", [(0, 8, 10, 0), (0, 0, 7, 0), (0, 0, 10, 32), (4, 0, 10, 0)])
def test_id_range_errors(args):
    with pytest.raises(ValueError):
        canid.id_base(*args)


def test_autopack_overflow_to_next_frame():
    # data area is 60 B -> 30 two-byte signals per frame
    p = proj(*[ch(i, 1, short_name=f"S{i}") for i in range(1, 32)])
    frames, errs = pack(p)
    assert not errs
    assert [len(f.placements) for f in frames] == [30, 1]
    assert [f.frame_idx for f in frames] == [0, 1]
    assert frames[0].placements[-1].offset == 62 and frames[0].dlc_bytes == 64 and frames[1].dlc_bytes == 8


def test_mixed_sizes_pack_tightly():
    p = proj(ch(1, 1, raw_bytes=1), ch(2, 2, raw_bytes=4), ch(3, 3, raw_bytes=2), ch(4, 4, raw_bytes=1))
    (f,), errs = pack(p)
    assert not errs and f.payload_bytes == 12  # 4 + 4 + 2 + 1 + 1, no gaps


def test_explicit_overlap_reported():
    p = proj(ch(1, 1, frame_idx=0, byte_offset=4), ch(2, 2, frame_idx=0, byte_offset=5))
    _, errs = pack(p)
    assert errs


def test_reserved_bytes_enforced():
    _, errs = pack(proj(ch(1, 1, frame_idx=0, byte_offset=2)))  # inside the timestamp
    assert errs
    _, errs = pack(proj(ch(1, 1, frame_idx=0, byte_offset=63)))  # 2 bytes past the end
    assert errs


def test_validation_rules():
    bad = proj(
        ch(1, 1), ch(1, 2, short_name="x y"),  # duplicate id, bad identifier
        ch(3, 1, short_name="S3"),  # duplicate pin
        ch(4, 21, sensor_type=SensorType.ACTIVE, short_name="S4"),  # analog type on digital pin
        ch(5, 3, sensor_type=SensorType.THERMOCOUPLE, pin_sig_neg=20, short_name="S5"),  # TC on AFE A
        ch(6, 2, fault_low=5, fault_high=1, short_name="S6"),
        ch(7, 4, sample_rate_hz=50, short_name="S7"),  # unsupported by hub fw
    )
    msgs = " | ".join(i.message for i in errors(validate(bad)))
    for frag in ["already used", "C identifier", "digital", "AFE B", "fault_low", "not supported"]:
        assert frag in msgs, frag


def test_afe_throughput():
    chans = [ch(i, pin, sensor_type=SensorType.ACTIVE, sample_rate_hz=100)
             for i, pin in enumerate([9, 18, 19, 20], start=1)]  # all on AFE B at 100 SPS
    msgs = [i.message for i in errors(validate(proj(*chans)))]
    assert any("SPS" in m for m in msgs)


def test_legacy_import_and_known_gear_position_issue():
    p = import_yaml(LEGACY_YAML)
    msgs = [str(i) for i in errors(validate(p))]
    assert msgs == [m for m in msgs if "Gear_Position" in m] and msgs  # only the known AFE B problem


def test_legacy_golden_register_values():
    """AFE register values must match what parse_cfg_to_c.py produced for the same YAML."""
    p = import_yaml(LEGACY_YAML)
    new = generate_hub(p, 0)
    old = LEGACY_C.read_text()

    def regs(c):
        return re.findall(r"\.(inpmux|pga|datarate|ref|idacmag|idacmux|vbias)\s*=\s*(0x[0-9A-Fa-f]+|\d+)", c)

    norm = lambda l: [(k, int(v, 0)) for k, v in l]
    assert norm(regs(new)) == norm(regs(old))
    for name in ("A", "B", "DIG"):
        cnt = lambda c: re.search(rf"{name}_ACTIVE_SENSORS_COUNT = (\d+)", c).group(1)
        assert cnt(new) == cnt(old)


def test_generated_c_standard_ids_and_digital_slots():
    new = generate_hub(import_yaml(LEGACY_YAML), 0)
    assert "0x00200000UL" in new  # hub 0, 200 Hz, idx 0
    dig = new.split("DIG_ACTIVE_SENSORS_CONFIG")[1]
    slots = [int(x) for x in re.findall(r"live_sensor_mapping = (\d+)", dig)]
    assert slots and all(s >= 20 for s in slots)  # digital slots no longer collide with analog slots


def test_dash_json_roundtrip():
    import json
    d = json.loads(generate_dash(import_yaml(LEGACY_YAML)))
    first = d["channels"][0]
    assert min(c["frame"]["byte_offset"] for c in d["channels"] if c["frame"]["id_base"] == 0x00200000) == 4  # after the timestamp
    assert first["frame"]["id_base"] == 0x00200000 and first["raw"] == {"bytes": 3, "signed": True, "endian": "little", "no_data_value": -8388608}
    assert d["config_hash"]


def test_connector_pins():
    from canharness import hwmap
    msgs = " | ".join(i.message for i in errors(validate(proj(
        ch(1, 26, short_name="a"), ch(2, 1, pin_5v=18, short_name="b"),
        ch(3, 2, pin_gnd=5, pin_gnd_connector=1, short_name="c"),
        ch(4, 3, sensor_type=SensorType.THERMOCOUPLE, pin_sig_neg=27, short_name="d"),
        ch(5, 4, pin_5v=3, pin_gnd=30, pin_gnd_connector=1, short_name="e")))))
    assert "not a signal input" in msgs and "5V pin 18" in msgs and "GND pin 5" in msgs
    assert hwmap.resolve_pin(27) == hwmap.resolve_pin(20)
    assert "signal (-)" not in msgs and "pin 3 " not in msgs
