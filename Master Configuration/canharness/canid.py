"""29-bit extended CAN ID standard from 'FSAE Sensor Hub CAN Tables.xlsx'.

The spreadsheet numbers bits MSB-first (bit 0 = ID bit 28):
  [0-1] type | [2-4] hub | [5-7] rate group | [8-12] frame idx | [13-20] frame counter | [21-28] other
The firmware fills counter/other at TX time; the app only defines the base and the RX filter mask.
"""

MSG_DATA, MSG_CONFIG, MSG_ACK, MSG_ERROR = 0, 1, 2, 3

RATE_CODES = {1000: 0b000, 200: 0b001, 100: 0b010, 50: 0b011, 10: 0b100}

ID_BITS = 29
# bits [0-12] of the spreadsheet numbering = ID bits 28..16
FILTER_MASK = 0x1FFF0000


def id_base(msg_type: int, hub: int, rate_hz: int, frame_idx: int) -> int:
    if not 0 <= msg_type <= 3:
        raise ValueError(f"message type {msg_type} out of range 0-3")
    if not 0 <= hub <= 7:
        raise ValueError(f"hub id {hub} out of range 0-7")
    if rate_hz not in RATE_CODES:
        raise ValueError(f"rate {rate_hz} Hz has no rate group (one of {sorted(RATE_CODES)})")
    if not 0 <= frame_idx <= 31:
        raise ValueError(f"frame index {frame_idx} out of range 0-31")
    return (msg_type << 27) | (hub << 24) | (RATE_CODES[rate_hz] << 21) | (frame_idx << 16)


def decode(can_id: int) -> dict:
    rate_by_code = {v: k for k, v in RATE_CODES.items()}
    return {
        "type": (can_id >> 27) & 0x3,
        "hub": (can_id >> 24) & 0x7,
        "rate_hz": rate_by_code.get((can_id >> 21) & 0x7),
        "frame_idx": (can_id >> 16) & 0x1F,
        "counter": (can_id >> 8) & 0xFF,
        "other": can_id & 0xFF,
    }
