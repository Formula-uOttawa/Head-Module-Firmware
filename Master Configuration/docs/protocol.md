# Head module configuration & debug protocol (v1)

All multi-byte values are **little-endian**. This document is the contract between
`canharness/` (Python), `Common/` (portable firmware C) and the CM7/CM4 glue code.

## 1. Transports

| Transport | Framing | Integrity |
|---|---|---|
| UART (ST-LINK VCP, UART4) | `0x00` + COBS(body) + `0x00` | CRC-16 in the body |
| UDP (future, via dash) | one message per datagram, body without CRC | UDP checksum |
| CAN-FD hub bus (head <-> hub) | `hcanlink`: body cut into <= 64 byte fragments, section 3.4 | CRC-16 in the body, CAN CRC per frame |

The message layer (section 3) is identical for every transport; a transport only moves
`message = type, seq, payload` between the host and `Hproto_Handle()`.

### UART framing
* A binary frame is `0x00`, then COBS-encoded `body`, then `0x00`. Send a leading `0x00` before every frame.
* `body = type(1) seq(1) payload(n) crc16(2)`; `crc16` = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection)
  over `type..payload`, stored little-endian. Max body 256 bytes (payload <= 252).
* The receiver is a small state machine: after any frame (or at start) a `0x00` byte starts a binary frame; any other
  first byte starts an **ASCII debug line** (terminated by `\n`) handled by the legacy text commands
  (`ping`, `send:...`, `sniff:...`, `reset`). Both can be interleaved on the same UART.
* Empty frames (`0x00 0x00`) are ignored (a `0x00` with nothing buffered keeps the receiver in binary mode, so repeated
  delimiters or a leading `0x00` before every frame are fine). A frame with a bad CRC or an oversize frame is dropped
  (counted) and the **next** `0x00`-delimited frame is received normally; after ~50 ms of silence in the middle of a
  frame the receiver resets. ASCII lines containing non-printable bytes are ignored, never executed.
* Slow operations: a COMMIT/CLEAR that erases flash can take over a second (the CM7 and its UART interrupt stall); hosts
  should allow ~20 s for those two requests.

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
| 3 | HUB_ROUTES | 16 | `u16 slot, u8 offset, u8 length, u8 is_signed, u8 flags, u8[2] rsvd, f32 multiplier, f32 offset`. `flags` bit0 = scaled: the channel (value type f32) is `(float)raw * multiplier + offset`; bit1 = no-data code: a received value of -8388608 (length >= 3, signed) is not stored, so the channel goes stale. An 8-byte element (older apps) means flags 0, no scaling |
| 4 | RX_FRAMES | 24 | `u8 bus, u8 extended, u8 dlc, u8 mux_enabled, u32 can_id, u32 id_mask, u32 timeout_ms, u16 first_signal, u16 signal_count, u8 mux_start_bit, u8 mux_bit_length, u16 rsvd` |
| 5 | RX_SIGNALS | 20 | `u16 slot, u8 start_bit, u8 bit_length, u8 big_endian, u8 is_signed, u8 is_float, u8 rsvd, i16 mux_value(-1 none), u16 rsvd, f32 multiplier, f32 offset` |
| 6 | BUSES | 12 | `u8 role(0 external,1 hub), u8 fd, u8 peripheral(1..3 = FDCAN1..3), u8 rsvd, u32 bitrate, u32 data_bitrate` |
| 7 | FILTERS | 12 | `u8 bus, u8 extended, u16 rsvd, u32 id, u32 mask` (mask bits set = must match) |
| 8 | TX_FRAMES | 16 | `u8 bus, u8 extended, u8 dlc, u8 rsvd, u32 can_id, u16 period_ms, u16 first_signal, u16 signal_count, u16 rsvd` (periodic transmit, e.g. to the Motec ECU) |
| 9 | TX_SIGNALS | 16 | `u16 slot, u8 start_bit, u8 bit_length, u8 big_endian, u8 is_signed, u8 is_float, u8 rsvd, f32 multiplier, f32 offset`; sent raw = round((value - offset) / multiplier), saturated to the field; float signals send the float32 of that quotient |

### 2.1 Hub config blob (format version 1)

Same container as above (32-byte header, `u16 id, u16 elem_size, u32 count` sections, zlib CRC-32 over the blob with the CRC field zeroed,
header `config_hash` = the project hash, header `total_len`), with its own magic `0x42554843` (bytes `C H U B`), so a head blob can never be
loaded into a hub and the other way round. Max 1024 bytes (24 sensors give 536). The app builds it for one hub id (`canharness/hubblob.py`);
the hub parses it with `hub_cfg.c` and rejects the whole blob on any violation.

| id | section | elem | layout |
|---|---|---|---|
| 0x21 | HUB_META | 8 | `u8 hub_id (0..7), u8[7] reserved` (exactly one element) |
| 0x22 | HUB_SENSORS | 20 | `u32 frame_id` (CAN id base: type, hub, rate, frame index; counter bits 15:8 must be 0), `u8 group` (1 AFE A, 2 AFE B, 3 digital), `u8 channel` (AFE input 0..9, or DIN 0..3), `u8 sensor_type` (0 resistive, 1 active, 2 thermocouple, 3 digital), `u8 rate` (0 10 Hz, 1 100 Hz, 2 200 Hz), `u8 byte_offset` (>= 4), `u8 byte_length` (1..4, end <= 64), `u8 slot` (connector pin - 1, unique, < 24), `u8 reserved`, `u8 inpmux, pga, datarate, ref, idacmag, idacmux, vbias` (ADS124S08 registers, 0 for digital), `u8 reserved` |

Row order inside a group is the AFE scan order. Hub-side checks: one sensor per (group, input) and per slot, digital group only with the
digital type and DIN 0..3, sensors that share a `frame_id` have the same rate and disjoint payload bytes. Unknown sections are skipped.

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
| 0x10 (hub) | LIVE_GET | `u16 first, u16 count` | Same layout as the head, but a slot is a connector pin (slot = pin - 1, 24 slots); flags: bit0 stale, bit1 no sensor configured on that pin (stale is also set). `first >= 24` or a count that does not fit is `out of range`. Analog slots hold the raw 24-bit ADC code **sign-extended to int32**; digital slots hold the pin level sampled at the 200 Hz tick |
| 0x02 | CFG_BEGIN | `u32 total_len, u32 blob_crc32` | `u16 max_chunk` |
| 0x03 | CFG_CHUNK | `u32 offset, bytes` | `u32 received` (offset must equal `received`; a retransmit of the immediately previous chunk with the same offset and length is acknowledged again, anything else out of order is `out of range`) |
| 0x04 | CFG_COMMIT | `u8 flags` (bit0 apply, bit1 store to flash) | `u8 applied, u8 stored, u64 hash, u8 store_err` (store_err is non-zero when the config was applied but the flash write failed afterwards: success reply with `stored=0`; older firmware sends 10 bytes). `flags` must be non-zero and contain only bits 0/1, else `bad request` |
| 0x05 | CFG_READ | `u8 source(0 active,1 stored), u32 offset, u16 max_len` | `u32 total_len, u32 offset, bytes` |
| 0x06 | CFG_CLEAR | `u8 flags` (bit0 also revert to defaults now; other bits are rejected). Erases the whole config flash sector. | - |
| 0x10 | LIVE_GET | `u16 first_slot, u16 count` (<= 32) | `u16 first, u16 count, count x (u32 value, u8 flags[bit0 stale], u16 rate_x10)`: `rate_x10` is the measured reception rate of the channel in 0.1 Hz, frames that carried a fresh sample per 2 s window (updated every 2 s; 0 = none in the last window). Older firmware sends 5-byte entries without the rate; hosts detect the entry size from the reply length |
| 0x11 | STATS_GET | - | `u8 buses, buses x (u32 rx_frames, u32 rx_matched, u32 rx_dropped, u32 fifo_lost, u8 bus_off, u8 passive), u8 8, 8 x u32 hub_lost, u8 buses, buses x (u32 tx_frames, u32 tx_dropped)` (the trailing transmit counters were added later; hosts treat them as optional) |
| 0x20 (hub) | ASCII | `ascii command line` (1..120 bytes, no terminator) | the text the console printed for it (<= 240 bytes). Same commands as the UART console (`ping`, `afe:probe:<a|b>`, `tcan:wd`, `diag:on`, `sniff:...`, `reset` ...), so a hub can be used through the head's CAN relay as if on its UART. Streams that commands start later (sniff output, `diag:on` lines) still go to the UART. `reset` resets after the reply |
| 0x30 | SNIFF_SET | `u8 bus, u8 enable` | - ; then unsolicited `0xB1` frames |
| 0xB1 | SNIFF_FRAME | (device -> host, seq 0) `u8 bus, u8 flags[bit0 ext, bit1 fd], u32 id, u32 ts_ms, u8 len, data` | - |
| 0x40 | HUB_RELAY | `u8 hub (0..7), u16 timeout_ms (0 = default 1000, max 1000), inner message = type, seq, payload of the hub protocol` (head only) | `u8 hub, inner reply = type|0x80 (or 0xFF), seq, payload`. The head sends the inner message to the hub as CONFIG frames and returns the hub's ACK frames (section 3.4). Errors: `timeout` (hub did not answer), `unsupported` (no hub bus in the active config), `busy` (another relay in flight / CM4 not ready), `out of range` (hub > 7 or message too big), `bad request`. The inner request is limited to 249 bytes, so hub chunks are 224 bytes. |
| 0x70 | REBOOT | `u32 magic = 0xB007F00D` (little endian; anything else is `bad request` and nothing happens) | - (reply sent first). Resets the MCU only; the SBC keeps its state. |

A config upload is: `CFG_BEGIN`, N x `CFG_CHUNK`, `CFG_COMMIT`. A new `CFG_BEGIN` discards any partial upload.

### 3.1 Fields appended by newer firmware (hosts must treat them as optional)
Head INFO (payload grows from 48 to 80 bytes): `48 u8 cm4_status (bit0 CAN buses down after a failed apply/rollback, bit1 boot apply still pending), 49 u8 boot_apply_rc, 50 u16 boot_apply_tries, 52 u32 hframe_crc_fail, 56 u32 hframe_overflow, 60 u32 hframe_desync, 64 u32 rx_ring_overflow, 68 u32 uart_err, 72 u32 uart_tx_fail, 76 u8 tcan_init_status, 77 u8[3] reserved`.
`tcan_init_status` bits (head and hub): 0x01 watchdog disable needed a retry, 0x02 watchdog could not be disabled, 0x04 init failed (head), 0x08 could not enter normal mode, 0x10 standby watchdog not disabled, 0x20 warm start (SBC already configured), 0x40 VCAN not in regulation, 0x80 CAN transceiver (CAN_CNTRL_1) not enabled.
Head STATS: after the per-bus transmit block, `u32 rx_nobus, u32 rx_ring_overrun, u32 rx_ring_high` (receive ring: frames on an unmapped bus, overruns, high-water mark).
Hub INFO (payload grows from 56 to 88 bytes; byte 9 `flags` bits: 0 CAN running, 1 acquisition running, 2 AFE rate/config bad, 3 SBC configured, 4 VCAN ok, 5 CAN scheduler config invalid, 6 AFE fault): `56 u32 tx_drop_full, 60 u32 afe_spi_err, 64 u32 afe_resyncs, 68 u32 afe_drdy_overrun, 72 u32 rx_overflow, 76 u32 uart_err, 80 u32 uart_tx_fail, 84 u8 tcan_init_status, 85 u8 afe_status (0x01/0x02 AFE A/B responds (ID matches and channel 0 registers read back), 0x04/0x08 running, 0x10/0x20 config ok, 0x80 SPI2 not in mode 1), 86 u8 inputs (bits 0-3 FAULT0-3, bit4 PG_5V, bit5 CAN_INT, raw levels), 87 reserved`.

Hub INFO (payload grows from 88 to 152 bytes; bytes 128..151 are per-AFE counters: `u32 samples_a, samples_b, drdy_edges_a, drdy_edges_b, resyncs_a, resyncs_b`; byte 86 `inputs` bits 6/7 are the DRDY_A / DRDY_B pin levels, idle high): `88 u64 active_hash, 96 u64 stored_hash (0 = nothing stored), 104 u8 active_source (0 compiled-in default, 1 flash), 105 u8 cfg_status (bits 3:0 = status of a stored blob rejected at boot: 1 too short, 2 bad magic, 3 unsupported version, 4 bad length, 5 crc, 6 bad section, 7 over limit, 8 value out of range; bit 7 = the compiled-in default is unusable), 106 u16 reserved, 108 u32 canrx_frames, 112 u32 canrx_msgs, 116 u32 canrx_bad, 120 u32 canrx_overrun, 124 u32 cantx_fail`.

### 3.2 Flash storage of the head config
Flash bank 1 sector 7 (`0x080E0000`, 128 KB) is an append-only log of 7 slots of 18432 bytes. A slot is a 32-byte header word (`magic 0x4C534C48, blob_len, blob_crc32, generation, check word`) followed by the blob (padded to 32 bytes). The blob body is programmed first and the header word last, so a power loss during a store leaves the previous config valid. The newest valid slot wins; a store only erases the sector when all slots are used (until the new header is written the head would fall back to the compiled-in default). Identical blobs are not stored again.

### 3.3 Value format of AFE channels
The hub sends the raw ADS124S08 24-bit two's-complement code, sign-extended to 32 bits, low `byte_length` bytes little endian (the app packs analog channels as 3 signed bytes). No offset or multiplier is applied on the hub. **The head converts analog hub channels to float32 volts** (HUB_ROUTES scaled flag): `volts = code / 2^23 * Vref / gain`, the voltage at the sensor pin; `Vref` is the project setting `afe_vref_v` (default 5 V, the external reference of the active and resistive channels, PGA gain 1) and 2.5 V (internal) with gain 128 for thermocouples. Digital channels stay integers. Calibration (dash) therefore works on volts for every analog channel, resistive ones included (a resistance comes from the calibration curve, not a special case); the dash JSON lists `volts_per_count` per analog channel. An analog channel that has no fresh sample (its AFE failed to start, stopped, or has not converted yet) is sent as the minimum 24-bit code, **-8388608** (`00 00 80` for 3 bytes), which a real conversion never reports; receivers treat it as "no data" (the dash JSON lists it as `no_data_value`). The CAN ID bits [15:8] carry an 8-bit frame counter (one per hub, incremented for every frame the hub transmits, including dropped ones, so receivers can count losses).

### 3.4 Config messages over the CAN-FD hub bus (`hcanlink.c`, shared by head and hub)

A message is the same `type, seq, payload` as on the UART. It is carried as the stream `u16 body_len, body` with `body = type, seq, payload,
crc16` (CRC-16/CCITT-FALSE over `type..payload`, little endian), cut into fragments of up to 64 bytes, one extended-ID CAN-FD frame (BRS)
each, the last padded with zeros to the next CAN-FD length (the receiver cuts at `body_len`). At most 258 stream bytes, i.e. 5 frames.

Identifier (the CAN ID standard of the sensor frames, only the fields used):

| bits | field |
|---|---|
| 28:27 | message type: 0 DATA (sensor frames), **1 CONFIG** (request, head -> hub), **2 ACK** (reply, hub -> head), 3 ERROR (unused) |
| 26:24 | hub id (the hub that is addressed / that answers) |
| 23:8 | 0 |
| 7:0 | fragment: bit 7 = last fragment, bits 6:0 = fragment index (0, 1, ...) |

Receiver rules: fragment 0 always starts a new message (an unfinished one is discarded); fragment `n` is only accepted after `n - 1`; the end
of the stream and the "last" flag must coincide; the CRC must match. Any violation drops the message silently (the head's relay then times
out and the app retries - every config message is idempotent or has a retransmit rule). A message whose fragments stop for 100 ms is
dropped. Hub side: a hardware filter accepts only CONFIG frames of the hub's own id (everything else on the bus is rejected, the hub's
3-element RX FIFO never sees the sensor traffic); the receive interrupt queues frames, the main loop reassembles, runs the same message handler as
the UART (`HubProto_Handle`) and sends the reply as ACK frames. Head side: the CM4 owns the CAN peripheral, so `HUB_RELAY` becomes the
mailbox command `HEAD_CMD_HUB_RELAY` (the request is acknowledged when the hub's reply is complete or after the timeout; the receive path
keeps running meanwhile) and the hub bus gets one extra hardware filter for ACK frames (message type bits only). One relay is in flight at
a time; the head's 1 s limit is below the CM7's 1.5 s mailbox wait.

## 4. Hub module

The hub speaks the same framing and message numbering on two transports: its UART (`hub_link.c`, bench; the ST-LINK VCP) and the CAN-FD
hub bus (`hub_canlink.c`, section 3.4; this is how the app reaches a hub through the head). Both call the same handler,
`hub_proto.c`. `hframe.c/.h` and `hcanlink.c/.h` are copies of `Common/` (a test fails if they drift). Implemented: IDENT, INFO,
LIVE_GET, CFG_BEGIN / CFG_CHUNK / CFG_COMMIT / CFG_READ / CFG_CLEAR, REBOOT and the ASCII debug commands (`ping`,
`send:tcan:<reg><val>` with a register whitelist, `sniff:<tcan|afea|afeb>:<on|off>`, `diag:on|off` for a periodic `hub:alive` status
line, `tcan:wd`, `reset`). STATS_GET and SNIFF_SET answer "unsupported".

**Hub config (same architecture as the head):** the app generates a hub blob (2.1); the hub parses it into RAM tables at boot from the
newest valid slot of its flash log (last 4 pages of flash bank 2, `hub_slots.h`: a ring of 2 KB pages; a store erases the next page,
programs the blob and writes the 32-byte header, magic `0x474C5348`, `len, crc32, generation, check word`, last, so an interrupted store leaves
the previous config valid), or from the compiled-in default (`hub_default_blob.c`, `generate --deploy-hub <id>`) if there is none.
A config is never changed while the hub runs: it takes effect after the next reset. Rules differing from the head:

* `CFG_COMMIT flags`: bit 1 (store) is mandatory, bit 0 (apply) = the hub resets itself right after the reply to boot the new config. `flags` without the store bit or with other bits are `bad request`. Reply: `u8 applied (reset scheduled), u8 stored (1), u64 hash, u8 0`.
* A blob is validated completely (`HubCfg_Parse`) before anything is written; an identical blob that is already the newest slot is not written again (`stored` = 1 without a flash access). Chunks are at most 224 bytes (`CFG_BEGIN` answers `max_chunk` 224); blobs up to 1024 bytes.
* `CFG_READ` source 0 = the active blob (flash slot or compiled-in default), 1 = the newest stored blob (`unsupported` if none). `CFG_CLEAR` erases the whole log (flag bit 0 is accepted and ignored); the running config is unchanged until the next reset, then the default is used.
* The hub id is part of the config. A blob for another id renames the hub after the reset (the old id no longer answers); the compiled-in default decides the id of a fresh board, so every board must be flashed with its own `hub_default_blob.c`.
* A stored blob that has a valid CRC but does not parse (e.g. a newer format) is ignored: the hub runs the default and reports it in `INFO` (`cfg_status`).

**Configuration ownership.** The project file in the app is the single source of truth. Both the head and the hubs are *targets* the app
writes to; neither is the authority. The app sends a hub its blob over the head's CAN relay (or directly over the hub's UART), each device
reports its active and stored config hash, and the hub's hash is the project hash, so an outdated hub is visible. Editing a hub directly
and propagating that to the head is deliberately not supported: the head derives everything it needs (frame layout, filters, channel
routing) from the project, so a hub-side edit would silently desynchronise it.
