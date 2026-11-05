# Head module configuration & debug protocol (v1)

All multi-byte values are **little-endian**. This document is the contract between
`canharness/` (Python), `Common/` (portable firmware C) and the CM7/CM4 glue code.

## 1. Transports

| Transport | Framing | Integrity |
|---|---|---|
| UART (ST-LINK VCP, UART4) | `0x00` + COBS(body) + `0x00` | CRC-16 in the body |
| UDP (future, via dash) | one message per datagram, body without CRC | UDP checksum |

The message layer (section 3) is identical for every transport; a transport only moves
`message = type, seq, payload` between the host and `Hproto_Handle()`.

### UART framing
* A binary frame is `0x00`, then COBS-encoded `body`, then `0x00`. Send a leading `0x00` before every frame.
* `body = type(1) seq(1) payload(n) crc16(2)`; `crc16` = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection)
  over `type..payload`, stored little-endian. Max body 256 bytes (payload <= 252).
* The receiver is a small state machine: after any frame (or at start) a `0x00` byte starts a binary frame; any other
  first byte starts an **ASCII debug line** (terminated by `\n`) handled by the legacy text commands
  (`ping`, `send:...`, `sniff:...`, `reset`). Both can be interleaved on the same UART.
* Empty frames (`0x00 0x00`) are ignored; a frame with a bad CRC is dropped silently.

## 2. Config blob (format version 1)

Header (32 bytes):

| off | type | field |
|---|---|---|
| 0 | u32 | magic `0x46434843` (bytes `C H C F`) |
| 4 | u16 | format_version (1) |
| 6 | u16 | header_len (32) |
| 8 | u32 | total_len (header + all sections) |
| 12 | u32 | section_count |
| 16 | u64 | config_hash (first 16 hex digits of the project hash) |
| 24 | u32 | reserved (0) |
| 28 | u32 | crc32 over the whole blob with this field zeroed (zlib/IEEE CRC-32) |

Each section: `u16 id, u16 elem_size, u32 count`, then `count * elem_size` bytes. Elements are padded so every
section is 4-byte aligned. A reader must accept `elem_size >= known size` (and ignore the extra bytes) and ignore unknown ids
(forward compatibility). Cross-references (`bus`, `slot`, `first_route`, ...) are indices into the named section.

| id | section | elem | layout |
|---|---|---|---|
| 1 | CHANNELS | 12 | `u16 channel_id, u8 value_type(0 u32,1 i32,2 f32), u8 source(0 hub,1 can_rx), u16 rate_hz, u16 rsvd, u32 timeout_ms`. Index = live-data slot |
| 2 | HUB_FRAMES | 16 | `u32 id_base, u32 id_mask, u8 hub, u8 min_len, u16 first_route, u16 route_count, u16 rsvd` |
| 3 | HUB_ROUTES | 8 | `u16 slot, u8 offset, u8 length, u8 is_signed, u8[3] rsvd` |
| 4 | RX_FRAMES | 24 | `u8 bus, u8 extended, u8 dlc, u8 mux_enabled, u32 can_id, u32 id_mask, u32 timeout_ms, u16 first_signal, u16 signal_count, u8 mux_start_bit, u8 mux_bit_length, u16 rsvd` |
| 5 | RX_SIGNALS | 20 | `u16 slot, u8 start_bit, u8 bit_length, u8 big_endian, u8 is_signed, u8 is_float, u8 rsvd, i16 mux_value(-1 none), u16 rsvd, f32 multiplier, f32 offset` |
| 6 | BUSES | 12 | `u8 role(0 external,1 hub), u8 fd, u8 peripheral(1..3 = FDCAN1..3), u8 rsvd, u32 bitrate, u32 data_bitrate` |
| 7 | FILTERS | 12 | `u8 bus, u8 extended, u16 rsvd, u32 id, u32 mask` (mask bits set = must match) |
| 8 | TX_FRAMES | 16 | `u8 bus, u8 extended, u8 dlc, u8 rsvd, u32 can_id, u16 period_ms, u16 first_signal, u16 signal_count, u16 rsvd` (periodic transmit, e.g. to the Motec ECU) |
| 9 | TX_SIGNALS | 16 | `u16 slot, u8 start_bit, u8 bit_length, u8 big_endian, u8 is_signed, u8 is_float, u8 rsvd, f32 multiplier, f32 offset`; sent raw = round((value - offset) / multiplier), saturated to the field; float signals send the float32 of that quotient |

Firmware limits (reported by `INFO`): 256 channels, 64 hub frames, 512 hub routes, 64 rx frames, 256 rx signals,
4 buses, 128 filters, 32 transmit frames, 128 transmit signals, blob <= 16384 bytes.

## 3. Messages

`message = type(1) seq(1) payload`. A response has `type | 0x80` and the same `seq`. A failed request returns type
`0xFF`: `u8 request_type, u8 error_code, [ascii text]`.

Error codes: 1 bad request, 2 busy/bad state, 3 too big, 4 crc mismatch, 5 invalid config, 6 flash error,
7 timeout (CM4 did not answer), 8 unsupported, 9 out of range.

| type | name | request payload | response payload |
|---|---|---|---|
| 0x01 | INFO | - | `u8 proto_ver, u8 blob_ver, u16 fw_version, u32 uptime_ms, u64 active_hash, u64 stored_hash, u8 active_source(0 default,1 flash,2 uploaded), u8 cm4_alive, u16 max_channels, u16 max_blob, u16 channel_count, u8[12] uid, u32 fdcan_kernel_hz` (the last field is absent on older firmware) |
| 0x07 | IDENT | - | `u8 device_type (1 head, 2 hub), u8 proto_ver, u16 fw_version, u8 instance (hub id, 0xFF none), u8 rsvd, u8[12] uid`. Sent first by the app to pick the matching screen; firmware older than IDENT answers "unsupported", which the app treats as a head. |
| 0x01 (hub) | INFO | - | Hub payload (56 B): `u8 proto_ver, u8 rsvd, u16 fw_version, u32 uptime_ms, u8 hub_id, u8 flags(bit0 CAN scheduler running), u8 a_count, u8 b_count, u8 dig_count, u8 tcan_int, u8 tec, u8 rec, u8 bus_off, u8 passive, u16 rsvd, u32 sysclk_hz, u32 fdcan_kernel_hz, u32 tx_ok, u32 tx_fail, u32 tx_skipped, u32 tx_invalid, u8[12] uid` |
| 0x10 (hub) | LIVE_GET | `u16 first, u16 count` | Same layout as the head, but a slot is a connector pin (slot = pin - 1, 24 slots); flags are always 0 |
| 0x02 | CFG_BEGIN | `u32 total_len, u32 blob_crc32` | `u16 max_chunk` |
| 0x03 | CFG_CHUNK | `u32 offset, bytes` | `u32 received` (offset must equal `received`) |
| 0x04 | CFG_COMMIT | `u8 flags` (bit0 apply, bit1 store to flash) | `u8 applied, u8 stored, u64 hash` |
| 0x05 | CFG_READ | `u8 source(0 active,1 stored), u32 offset, u16 max_len` | `u32 total_len, u32 offset, bytes` |
| 0x06 | CFG_CLEAR | `u8 flags` (bit0 also revert to defaults now) | - |
| 0x10 | LIVE_GET | `u16 first_slot, u16 count` (<= 48) | `u16 first, u16 count, count x (u32 value, u8 flags[bit0 stale])` |
| 0x11 | STATS_GET | - | `u8 buses, buses x (u32 rx_frames, u32 rx_matched, u32 rx_dropped, u32 fifo_lost, u8 bus_off, u8 passive), u8 8, 8 x u32 hub_lost, u8 buses, buses x (u32 tx_frames, u32 tx_dropped)` (the trailing transmit counters were added later; hosts treat them as optional) |
| 0x30 | SNIFF_SET | `u8 bus, u8 enable` | - ; then unsolicited `0xB1` frames |
| 0xB1 | SNIFF_FRAME | (device -> host, seq 0) `u8 bus, u8 flags[bit0 ext, bit1 fd], u32 id, u32 ts_ms, u8 len, data` | - |
| 0x40-0x4F | HUB_CFG_* | reserved: head relays a hub config blob over CAN (`CONFIG` message type) | |
| 0x70 | REBOOT | - | - (reply sent first) |

A config upload is: `CFG_BEGIN`, N x `CFG_CHUNK`, `CFG_COMMIT`. A new `CFG_BEGIN` discards any partial upload.

## 4. Hub module

The hub speaks the same framing and message numbering (`SensorHubV1/Core/Src/hub_proto.c`, `hub_link.c`; `hframe.c/.h` are
copies of `Common/`, a test fails if they drift). Implemented: IDENT, INFO, LIVE_GET, REBOOT and the ASCII debug commands.
CFG_*, STATS_GET and SNIFF_SET answer "unsupported" until hub runtime config exists.

**Configuration ownership.** The project file in the app is the single source of truth. Both the head and the hubs are
*targets* the app writes to; neither is the authority. Hub configuration is generated for the hub (compiled-in today,
a binary blob later), loaded over its own UART for bench work or pushed by the head over CAN (`0x40-0x4F`, reserved).
Each device reports its active config hash, so the app and the head can see which devices run an outdated config.
Editing a hub directly and propagating that to the head is deliberately not supported: the head derives everything it
needs (frame layout, filters, channel routing) from the project, so a hub-side edit would silently desynchronise it.
