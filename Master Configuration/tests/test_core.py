from pathlib import Path

import pytest

from canharness import canid
from canharness.generators.dash_json import generate_dash
from canharness import hubblob
from canharness.generators.hub_c import hub_sensor_rows
from canharness.legacy import import_yaml
from canharness.model import Channel, Hub, Project, SensorType
from canharness.packing import pack
from canharness.validate import errors, validate

HUB_DIR = Path(__file__).resolve().parents[3] / "SensorHubV1"
LEGACY_YAML = HUB_DIR / "config" / "rear_sensor_config.yaml"


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


def test_packing_keeps_to_the_data_area_and_never_overlaps():
    p = proj(*[ch(i, i, raw_bytes=4) for i in range(1, 40)])
    frames, errs = pack(p)
    assert not errs
    for f in frames:
        spans = sorted((x.offset, x.offset + x.length) for x in f.placements)
        assert spans[0][0] >= p.settings.data_start and spans[-1][1] <= p.settings.data_end
        assert all(a[1] <= b[0] for a, b in zip(spans, spans[1:]))


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


# ADS124S08 registers (INPMUX, PGA, DATARATE, REF, IDACMAG, IDACMUX, VBIAS) parse_cfg_to_c.py produced for the legacy YAML, AFE A
# channels first (8, active), then AFE B (4 resistive, 5 thermocouples); the digital inputs carry no registers
GOLDEN_REGS = [(156, 0, 29, 0, 15, 255, 0), (124, 0, 29, 0, 15, 255, 0), (92, 0, 29, 0, 15, 255, 0), (60, 0, 29, 0, 15, 255, 0),
               (28, 0, 29, 0, 15, 255, 0), (140, 0, 29, 0, 15, 255, 0), (108, 0, 29, 0, 15, 255, 0), (12, 0, 29, 0, 15, 255, 0),
               (124, 8, 23, 1, 7, 247, 0), (140, 8, 23, 1, 7, 248, 0), (108, 8, 23, 1, 7, 246, 0), (156, 8, 23, 1, 7, 249, 0),
               (83, 15, 23, 9, 15, 255, 32), (3, 15, 23, 9, 15, 255, 1), (67, 15, 23, 9, 15, 255, 16), (19, 15, 23, 9, 15, 255, 2),
               (35, 15, 23, 9, 15, 255, 4)]


def test_legacy_golden_register_values():
    """AFE register values must match what parse_cfg_to_c.py produced for the same YAML."""
    rows = hub_sensor_rows(import_yaml(LEGACY_YAML), 0)
    analog = [r for r in rows if r[1] in (1, 2)]
    assert [tuple(r[8:15]) for r in analog] == GOLDEN_REGS
    assert [sum(1 for r in rows if r[1] == g) for g in (1, 2, 3)] == [8, 9, 2]


def test_hub_blob_standard_ids_and_digital_slots():
    p = import_yaml(LEGACY_YAML)
    rows = hub_sensor_rows(p, 0)
    assert rows[0][0] == 0x00200000  # hub 0, 200 Hz, idx 0
    assert all(r[7] >= 20 for r in rows if r[1] == 3)  # digital slots do not collide with analog slots
    parsed = hubblob.parse(hubblob.build(p, 0))
    assert parsed["hub_id"] == 0 and len(parsed["sensors"]) == len(rows) and parsed["config_hash"] == int(p.config_hash(), 16)


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


def test_analog_values_are_volts():
    """Every analog hub input is volts (resistance comes from the calibration); the code scale follows the reference."""
    from canharness import hwmap
    assert hwmap.volts_per_count("active", 5.0) == pytest.approx(5.0 / 2**23)
    assert hwmap.volts_per_count("resistive", 5.0) == pytest.approx(5.0 / 2**23)
    assert hwmap.volts_per_count("thermocouple", 5.0) == pytest.approx(2.5 / 128 / 2**23)     # internal reference, gain 128
