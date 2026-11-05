"""Project data model (pydantic). The project file is the single source of truth."""

from __future__ import annotations

import hashlib
import json
from enum import Enum
from pathlib import Path
from typing import Literal, Optional

from pydantic import BaseModel, Field, field_validator

from . import SCHEMA_VERSION


def timeout_for_rate(rate_hz: float) -> int:
    """Default staleness timeout for a hub channel: 5 missed frames, at least 50 ms."""
    return max(50, int(5000 / rate_hz))


class SensorType(str, Enum):
    RESISTIVE = "resistive"
    ACTIVE = "active"
    THERMOCOUPLE = "thermocouple"
    DIGITAL = "digital"
    CAN = "can"  # decoded by the head from an external CAN frame


class Scaling(BaseModel):
    """eng = raw * gain + offset (applied by the dash; the dash may use the calibration curve instead)."""

    gain: float = 1.0
    offset: float = 0.0


class Channel(BaseModel):
    channel_id: int
    name: str
    short_name: str
    source: Literal["hub", "can_rx"] = "hub"
    hub: Optional[int] = None
    sensor_type: SensorType = SensorType.ACTIVE
    sample_rate_hz: int = 10
    units: str = ""
    scaling: Scaling = Field(default_factory=Scaling)
    fault_low: Optional[float] = None
    fault_high: Optional[float] = None
    # sensor connector pins (J-sensor). sig_neg None means board ground (single ended)
    pin_sig_pos: Optional[int] = None
    pin_sig_neg: Optional[int] = None
    # 5V/GND connector pins, information only
    pin_5v: Optional[int] = None
    pin_gnd: Optional[int] = None
    pin_gnd_connector: Literal[1, 2] = 2  # GND may be taken from either connector
    cal_x: list[float] = Field(default_factory=list)
    cal_y: list[float] = Field(default_factory=list)
    raw_bytes: int = 2
    signed: bool = False
    # explicit frame placement; None = auto-pack
    frame_idx: Optional[int] = None
    byte_offset: Optional[int] = None
    enabled: bool = True
    notes: str = ""


class Hub(BaseModel):
    hub_id: int
    name: str
    location: str = ""


class Bus(BaseModel):
    """A CAN bus on the head module. Exactly one bus has role 'hub' (carries all hub data frames)."""

    id: str  # C identifier, used for HEAD_BUS_<ID>
    name: str
    role: Literal["hub", "external"] = "external"
    type: Literal["can", "can_fd"] = "can"
    bitrate: int = 500_000
    data_bitrate: Optional[int] = None
    head_peripheral: str = ""  # e.g. FDCAN2
    # hardware filter elements configured in CubeMX (FDCAN StdFiltersNbr / ExtFiltersNbr)
    std_filters: int = 28
    ext_filters: int = 8


def default_buses() -> list["Bus"]:
    return [
        # 1.6 Mbit/s data: 3.2 Mbit/s cannot be generated exactly from the head's 88 MHz kernel clock (hub: 51.2 MHz)
        Bus(id="HUB", name="Hub bus", role="hub", type="can_fd", bitrate=320_000, data_bitrate=1_600_000,
            head_peripheral="FDCAN1", std_filters=0, ext_filters=16),
        Bus(id="EXT1", name="External CAN 1", role="external", type="can", bitrate=500_000,
            head_peripheral="FDCAN2", std_filters=16, ext_filters=8),
    ]


class RxSignal(BaseModel):
    channel_id: int  # a Channel with source == "can_rx"
    start_bit: int
    bit_length: int
    byte_order: Literal["intel", "motorola"] = "intel"
    signed: bool = False
    is_float: bool = False
    multiplier: float = 1.0
    offset: float = 0.0
    mux_value: Optional[int] = None


class RxFrame(BaseModel):
    name: str
    bus: str
    can_id: int
    extended: bool = False
    id_mask: Optional[int] = None
    dlc: int = 8
    rate_hz: Optional[float] = None
    timeout_ms: int = 1000
    mux_start_bit: Optional[int] = None  # Intel bit numbering
    mux_bit_length: Optional[int] = None
    signals: list[RxSignal] = Field(default_factory=list)


class TxSignal(BaseModel):
    """A head channel value sent to an external device (e.g. the Motec ECU).

    The receiver reconstructs value = raw * multiplier + offset, so the head sends raw = round((value - offset) / multiplier).
    """

    channel_id: int  # any enabled channel (hub or can_rx)
    start_bit: int
    bit_length: int = 16
    byte_order: Literal["intel", "motorola"] = "motorola"  # Motec's usual default is big-endian
    signed: bool = False
    is_float: bool = False
    multiplier: float = 1.0
    offset: float = 0.0


class TxFrame(BaseModel):
    """A periodic frame the head transmits on a bus."""

    name: str
    bus: str
    can_id: int
    extended: bool = False
    dlc: int = 8
    rate_hz: float = 50
    signals: list[TxSignal] = Field(default_factory=list)


class Settings(BaseModel):
    data_start: int = 4  # bytes 0-3 are the timestamp
    data_end: int = 64  # exclusive; full CAN-FD payload
    supported_rates_hz: list[int] = Field(default_factory=lambda: [10, 100, 200])
    # FDCAN kernel clocks the boards are (to be) configured with. 80 MHz generates 320 kbit/s, 500 kbit/s and 5 Mbit/s exactly.
    # The head reports its real clock in INFO, so the Device tab warns if the board differs from this setting.
    head_fdcan_kernel_hz: int = 80_000_000
    hub_fdcan_kernel_hz: int = 80_000_000
    udp_ip: str = "192.168.1.10"
    udp_port: int = 5005


class Project(BaseModel):
    schema_version: int = SCHEMA_VERSION
    name: str = "CAN-harness"
    settings: Settings = Field(default_factory=Settings)
    hubs: list[Hub] = Field(default_factory=list)
    buses: list[Bus] = Field(default_factory=default_buses)
    channels: list[Channel] = Field(default_factory=list)
    rx_frames: list[RxFrame] = Field(default_factory=list)
    tx_frames: list[TxFrame] = Field(default_factory=list)

    @field_validator("buses")
    @classmethod
    def _default_buses(cls, v):  # projects saved before buses existed
        return v or default_buses()

    def config_hash(self) -> str:
        blob = json.dumps(self.model_dump(mode="json"), sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(blob.encode()).hexdigest()[:16]

    def save(self, path: Path) -> None:
        path.write_text(self.model_dump_json(indent=2) + "\n", encoding="utf-8")

    @classmethod
    def load(cls, path: Path) -> "Project":
        return cls.model_validate_json(path.read_text(encoding="utf-8"))
