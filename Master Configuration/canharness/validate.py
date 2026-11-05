"""Project validation. Errors block generation; warnings do not."""

from __future__ import annotations

import re
from dataclasses import dataclass

from . import canid, cantiming, hwmap, packing, rxdecode
from .model import Project, SensorType

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MAX_NOMINAL_BITRATE = 1_000_000  # mirrored in Common/Src/head_cfg.c
MAX_DATA_BITRATE = 8_000_000


@dataclass
class Issue:
    level: str  # "error" | "warning"
    where: str
    message: str

    def __str__(self) -> str:
        return f"{self.level.upper()} [{self.where}] {self.message}"


def validate(project: Project) -> list[Issue]:
    issues: list[Issue] = []

    def err(where, msg):
        issues.append(Issue("error", where, msg))

    def warn(where, msg):
        issues.append(Issue("warning", where, msg))

    hub_ids = {h.hub_id for h in project.hubs}
    for h in project.hubs:
        if not 0 <= h.hub_id <= 7:
            err(f"hub {h.hub_id}", "hub id must be 0-7 (3 bits in the CAN ID)")
    if len(hub_ids) != len(project.hubs):
        err("hubs", "duplicate hub ids")

    seen_id: dict[int, str] = {}
    seen_name: dict[str, str] = {}
    pins: dict[tuple[int, int], str] = {}
    afe_load: dict[tuple[int, int], list] = {}

    for ch in project.channels:
        w = ch.short_name or str(ch.channel_id)
        if ch.channel_id in seen_id:
            err(w, f"channel_id {ch.channel_id} already used by {seen_id[ch.channel_id]}")
        seen_id[ch.channel_id] = w
        if not IDENT.match(ch.short_name):
            err(w, "short_name must be a valid C identifier")
        if ch.short_name in seen_name:
            err(w, "duplicate short_name")
        seen_name[ch.short_name] = w
        if ch.fault_low is not None and ch.fault_high is not None and ch.fault_low >= ch.fault_high:
            err(w, "fault_low must be below fault_high")
        if len(ch.cal_x) != len(ch.cal_y):
            err(w, "calibration X and Y must be the same length")
        elif any(b <= a for a, b in zip(ch.cal_x, ch.cal_x[1:])):
            err(w, "calibration X must be strictly increasing")
        if not ch.enabled or ch.source != "hub":
            continue
        if ch.sensor_type == SensorType.CAN:
            err(w, "sensor_type 'can' requires source 'can_rx'")
            continue
        if ch.hub not in hub_ids:
            err(w, f"hub {ch.hub} is not defined")
            continue
        if ch.sample_rate_hz not in canid.RATE_CODES:
            err(w, f"{ch.sample_rate_hz} Hz has no rate group in the CAN ID standard")
        elif ch.sample_rate_hz not in project.settings.supported_rates_hz:
            err(w, f"{ch.sample_rate_hz} Hz is not supported by the hub firmware ({project.settings.supported_rates_hz})")
        if not 1 <= ch.raw_bytes <= 4:
            err(w, "raw_bytes must be 1-4")
        elif ch.sensor_type != SensorType.DIGITAL and (ch.raw_bytes < 3 or not ch.signed):
            # the hub sends the raw 24-bit two's complement ADC code (sign-extended, low bytes first): anything shorter wraps
            err(w, "analog channels carry the raw 24-bit signed ADC code: set raw_bytes to 3 (or 4) and signed")
        if ch.pin_sig_pos is None:
            err(w, "signal (+) pin missing")
            continue
        if ch.pin_sig_pos in hwmap.TC_NEG_PINS[1:] or ch.pin_sig_pos in hwmap.CONN1_GND_PINS:
            err(w, f"connector 1 pin {ch.pin_sig_pos} is not a signal input")
            continue
        try:
            afe, afe_pin = hwmap.resolve_pin(ch.pin_sig_pos)
        except ValueError as e:
            err(w, str(e))
            continue
        key = (ch.hub, ch.pin_sig_pos)
        if key in pins:
            err(w, f"signal pin {ch.pin_sig_pos} already used by {pins[key]}")
        pins[key] = w
        if (ch.sensor_type == SensorType.DIGITAL) != (afe == 3):
            err(w, "digital sensors must use digital input pins and vice versa")
            continue
        if ch.pin_5v is not None and ch.pin_5v not in hwmap.CONN2_5V_PINS:
            err(w, f"5V pin {ch.pin_5v} must be connector 2 pin 1-17 (info only)")
        if ch.pin_gnd is not None:
            ok = hwmap.CONN2_GND_PINS if ch.pin_gnd_connector == 2 else hwmap.CONN1_GND_PINS
            if ch.pin_gnd not in ok:
                err(w, f"GND pin {ch.pin_gnd} is not a GND pin on connector {ch.pin_gnd_connector} (info only)")
        if afe == 3:
            continue
        try:
            neg = hwmap.AINCOM
            if ch.pin_sig_neg is not None:
                if ch.pin_sig_neg not in hwmap.TC_NEG_PINS:
                    raise ValueError(f"signal (-) pin must be one of {hwmap.TC_NEG_PINS} (shared TC-) or empty (GND)")
                _, neg = hwmap.resolve_pin(ch.pin_sig_neg)
            st = ch.sensor_type.value
            hwmap.inpmux(st, afe_pin, neg)
            hwmap.datarate(afe, st)
            hwmap.vbias(st, afe_pin)
        except (ValueError, KeyError) as e:
            err(w, str(e))
        afe_load.setdefault((ch.hub, afe), []).append(ch)

    for (hub, afe), chans in afe_load.items():
        per_ch = hwmap.AFE_SPS[afe] / len(chans)
        for ch in chans:
            if per_ch < ch.sample_rate_hz:
                err(ch.short_name, f"AFE {'AB'[afe - 1]} on hub {hub}: {len(chans)} channels at "
                    f"{hwmap.AFE_SPS[afe]} SPS gives ~{per_ch:.0f} Hz per channel, below {ch.sample_rate_hz} Hz")

    _validate_rx(project, err, warn)
    _validate_tx(project, err, warn)
    _, pack_errors = packing.pack(project)
    for e in pack_errors:
        err("packing", e)
    return issues


def errors(issues: list[Issue]) -> list[Issue]:
    return [i for i in issues if i.level == "error"]


def _validate_rx(project: Project, err, warn) -> None:
    buses = {b.id: b for b in project.buses}
    chans = {c.channel_id: c for c in project.channels}
    hub_buses = [b for b in project.buses if b.role == "hub"]
    if len(hub_buses) != 1:
        err("buses", f"exactly one bus must have role 'hub' (found {len(hub_buses)})")
    for b in project.buses:
        if not IDENT.match(b.id):
            err(f"bus {b.id}", "bus id must be a valid C identifier")
        if b.head_peripheral.upper() not in ("FDCAN1", "FDCAN2"):  # the H745 has two FDCAN controllers
            err(f"bus {b.id}", "head_peripheral must be FDCAN1 or FDCAN2")
        if b.type == "can_fd" and b.head_peripheral.upper() == "FDCAN2":  # CubeMX gives FDCAN2 8-byte message RAM elements
            err(f"bus {b.id}", "FDCAN2 has 8-byte message RAM elements in the CubeMX setup, so it can only be a classic CAN bus (use FDCAN1 for CAN-FD)")
        if b.bitrate > MAX_NOMINAL_BITRATE or (b.data_bitrate or 0) > MAX_DATA_BITRATE:  # firmware rejects these too
            err(f"bus {b.id}", f"bit rates are limited to {MAX_NOMINAL_BITRATE // 1_000_000} Mbit/s nominal and {MAX_DATA_BITRATE // 1_000_000} Mbit/s data")
        if b.bitrate <= 0 or (b.type == "can_fd" and not b.data_bitrate):
            err(f"bus {b.id}", "bit rate(s) must be set (CAN-FD buses need a data bit rate)")
        else:  # every device on the bus must be able to generate the rates exactly from its own FDCAN kernel clock
            devices = [("head", project.settings.head_fdcan_kernel_hz)]
            if b.role == "hub":
                devices.append(("hub", project.settings.hub_fdcan_kernel_hz))
            for dev, clk in devices:
                for label, rate, data in (("nominal", b.bitrate, False), ("data", b.data_bitrate, True)):
                    if rate and (b.type == "can_fd" or not data) and not cantiming.achievable(clk, rate, data):
                        alt = ", ".join(cantiming.fmt(r) for r in cantiming.nearby(clk, rate, data))
                        err(f"bus {b.id}", f"{label} rate {cantiming.fmt(rate)} cannot be generated exactly by the {dev} "
                            f"({clk / 1e6:g} MHz FDCAN clock). Achievable nearby: {alt}")
    if len(buses) != len(project.buses):
        err("buses", "duplicate bus ids")

    seen_frame: dict[tuple[str, int, bool], str] = {}
    mapped: dict[int, str] = {}
    for f in project.rx_frames:
        w = f.name
        b = buses.get(f.bus)
        if b is None:
            err(w, f"bus '{f.bus}' is not defined")
            continue
        if b.role == "hub":
            err(w, "custom receive frames cannot be on the hub bus")
        max_id = 0x1FFFFFFF if f.extended else 0x7FF
        if not 0 <= f.can_id <= max_id:
            err(w, f"CAN ID 0x{f.can_id:X} out of range for {'extended' if f.extended else 'standard'} ID")
        if f.id_mask is not None and not 0 <= f.id_mask <= max_id:
            err(w, "id_mask out of range")
        max_dlc = 64 if b.type == "can_fd" else 8
        if not 0 <= f.dlc <= max_dlc or (f.dlc > 8 and f.dlc not in packing.FD_LENGTHS):
            err(w, f"DLC {f.dlc} not valid on a {b.type} bus")
        key = (f.bus, f.can_id, f.extended)
        if key in seen_frame:
            err(w, f"duplicate CAN ID 0x{f.can_id:X} on bus {f.bus} (also {seen_frame[key]})")
        seen_frame[key] = w
        if f.timeout_ms <= 0:
            err(w, "timeout_ms must be positive")
        if (f.mux_start_bit is None) != (f.mux_bit_length is None):
            err(w, "mux_start_bit and mux_bit_length must be set together")
        if f.mux_start_bit is not None and f.mux_start_bit + f.mux_bit_length > 8 * f.dlc:
            err(w, "multiplexer field extends beyond the DLC")
        used: dict[int, tuple[str, object]] = {}
        for s in f.signals:
            ch = chans.get(s.channel_id)
            sw = f"{w}/{ch.short_name if ch else s.channel_id}"
            if ch is None:
                err(sw, f"channel {s.channel_id} does not exist")
                continue
            if ch.source != "can_rx":
                err(sw, "channel must have source 'can_rx'")
            if s.channel_id in mapped:
                err(sw, f"channel already decoded from {mapped[s.channel_id]}")
            mapped[s.channel_id] = w
            if not 1 <= s.bit_length <= 32:
                err(sw, "bit_length must be 1-32")
                continue
            if s.is_float and s.bit_length != 32:
                err(sw, "float signals must be 32 bits")
            if s.mux_value is not None and f.mux_start_bit is None:
                err(sw, "signal has a mux value but the frame has no multiplexer")
            bits = rxdecode.bit_span(s)
            if s.start_bit < 0 or max(bits) >= 8 * f.dlc:
                err(sw, f"bits extend beyond the {f.dlc}-byte payload")
            for bit in bits:
                other = used.get(bit)
                if other and (other[1] is None or s.mux_value is None or other[1] == s.mux_value):
                    err(sw, f"overlaps signal {other[0]}")
                    break
            for bit in bits:
                used.setdefault(bit, (sw, s.mux_value))
    for ch in project.channels:
        if ch.enabled and ch.source == "can_rx" and ch.channel_id not in mapped:
            warn(ch.short_name, "can_rx channel is not decoded by any receive frame")

    from .generators.head_c import head_tables, plan_filters  # late import: generators depend on the model
    from .blob import LIMITS, MAX_BLOB
    if len(project.buses) > LIMITS["buses"]:
        err("buses", f"firmware supports at most {LIMITS['buses']} buses")
    elif not errors_in_buses(project):
        tables = head_tables(project)
        for name, limit in LIMITS.items():
            if len(tables[name]) > limit:
                err("head config", f"{len(tables[name])} {name} exceeds firmware limit {limit}")
    for bus_id, plan in plan_filters(project).items():
        b = buses[bus_id]
        if plan["std_needed"] > b.std_filters:
            err(f"bus {bus_id}", f"needs {plan['std_needed']} standard filters, only {b.std_filters} configured")
        if plan["ext_needed"] > b.ext_filters:
            err(f"bus {bus_id}", f"needs {plan['ext_needed']} extended filters, only {b.ext_filters} configured")


def _validate_tx(project: Project, err, warn) -> None:
    buses = {b.id: b for b in project.buses}
    chans = {c.channel_id: c for c in project.channels if c.enabled}
    seen: dict[tuple[str, int, bool], str] = {}
    rx_ids = {(f.bus, f.can_id, f.extended) for f in project.rx_frames}
    names = set()
    for f in project.tx_frames:
        w = f.name
        if w in names:
            err(w, "duplicate transmit frame name")
        names.add(w)
        b = buses.get(f.bus)
        if b is None:
            err(w, f"bus '{f.bus}' is not defined")
            continue
        if b.role == "hub":
            err(w, "transmit frames cannot be on the hub bus")
        max_id = 0x1FFFFFFF if f.extended else 0x7FF
        if not 0 <= f.can_id <= max_id:
            err(w, f"CAN ID 0x{f.can_id:X} out of range for {'extended' if f.extended else 'standard'} ID")
        max_dlc = 64 if b.type == "can_fd" else 8
        if not 1 <= f.dlc <= max_dlc or (f.dlc > 8 and f.dlc not in packing.FD_LENGTHS):
            err(w, f"DLC {f.dlc} not valid on a {b.type} bus")
        if not 1 <= f.rate_hz <= 1000:
            err(w, "rate must be 1-1000 Hz")
        key = (f.bus, f.can_id, f.extended)
        if key in seen:
            err(w, f"duplicate CAN ID 0x{f.can_id:X} on bus {f.bus} (also {seen[key]})")
        seen[key] = w
        if key in rx_ids:
            warn(w, f"CAN ID 0x{f.can_id:X} on bus {f.bus} is also a receive frame")
        used: dict[int, str] = {}
        for s in f.signals:
            ch = chans.get(s.channel_id)
            sw = f"{w}/{ch.short_name if ch else s.channel_id}"
            if ch is None:
                err(sw, f"channel {s.channel_id} does not exist or is disabled")
                continue
            if not 1 <= s.bit_length <= 32:
                err(sw, "bit_length must be 1-32")
                continue
            if s.is_float and s.bit_length != 32:
                err(sw, "float signals must be 32 bits")
            if s.multiplier == 0:
                err(sw, "multiplier must not be 0")
            bits = rxdecode.bit_span(s)
            if s.start_bit < 0 or max(bits) >= 8 * f.dlc:
                err(sw, f"bits extend beyond the {f.dlc}-byte payload")
            clash = next((used[bit] for bit in bits if bit in used), None)
            if clash:
                err(sw, f"overlaps signal {clash}")
            for bit in bits:
                used.setdefault(bit, sw)


def errors_in_buses(project: Project) -> bool:
    """True if bus definitions are too broken for table generation (a hub bus must exist)."""
    return sum(b.role == "hub" for b in project.buses) != 1 or any(
        f.bus not in {b.id for b in project.buses} for f in (*project.rx_frames, *project.tx_frames))
