# Review: Common/ + CM4 user code (read-only)

Paths are relative to the HeadModuleV1 root. Line numbers refer to the files as read today.
Short names: HC = Common/Src, CM4 = CM4/Core/Src.

## HIGH

1. **HC-1 HIGH - no CAN bus-off recovery anywhere** (CM4/Core/Src/head_cm4.c:319-327, whole file)
   - Defect: bus-off is only read into stats every 100 ms. Nothing restarts the peripheral and the bus-off interrupt is not enabled.
   - Scenario: a wiring glitch or short drives TEC over 255. The M_CAN core sets CCCR.INIT in hardware. Software must clear it again, and HAL only does that in `HAL_FDCAN_Start()` (hal_fdcan.c:745). The head then stays silent: no RX, no TX. TX FIFO fills and `tx_dropped` climbs. Only a reboot or a new config apply recovers.
   - Fix: in the 100 ms block, if `ps.BusOff`, call `HAL_FDCAN_Stop()` then `HAL_FDCAN_Start()`, re-arm the notifications, and count recoveries.

2. **HC-2 HIGH - runtime sniff enable/disable never reaches the hardware** (CM4/Core/Src/head_cm4.c:74-78, 300-308)
   - Defect: `set_global_filter()` calls `HAL_FDCAN_ConfigGlobalFilter()`, which returns HAL_ERROR unless `State == READY` (hal_fdcan.c:1954+). A started bus is `BUSY`. The result is cast to `(void)`, and `s_sniff_applied` is updated regardless, so it is never retried.
   - Scenario: the app sends SNIFF_SET for bus 1 while the buses run. `sniff_mask` changes, the GFC register does not, and the global filter still rejects non-matching frames. The sniffer shows only frames already accepted by the configured filters, and "define frame from sniffed ID" cannot work for unknown IDs. It only works if `sniff_mask` was set before `can_configure()`.
   - Fix: apply by `HAL_FDCAN_Stop()` -> ConfigGlobalFilter -> `HAL_FDCAN_Start()` (and re-activate notifications). Check the return value and only update `s_sniff_applied` on success.

3. **HC-3 HIGH - FDCAN2 message RAM elements are fixed at 8 bytes by CubeMX, but the blob can request FD or DLC up to 64 on any peripheral** (CM4/Core/Src/head_cm4.c:117-132; main.c MX_FDCAN2_Init: RxFifo0ElmtSize/TxElmtSize = 8 B; head_cfg.c:147,172)
   - Defect: `can_configure()` never sets `RxFifo0ElmtSize`/`TxElmtSize`, so FDCAN2 keeps 8-byte elements. `HeadCfg_Parse` accepts `fd=1` on any peripheral and tx `dlc` 1..64 on any bus.
   - Scenario A: the blob sets peripheral 2 with `fd=1`, or a tx frame with dlc > 8 on a classic bus. `HAL_FDCAN_AddMessageToTxFifoQ` copies DLC-many bytes into a 4-word element and overwrites the neighbouring TX elements and the next RAM block (copy loop in hal_fdcan.c, `FDCAN_CopyMessageToRAM`).
   - Scenario B: a classic frame with DLC code 9-15 arrives on FDCAN2. `dlc_to_len()` returns 12-64 and HAL reads past the 8-byte element into the following elements. The garbage is decoded and sniffed as payload.
   - STATUS.md says "up to 8 B on classic buses", but nothing in firmware enforces it.
   - Fix: in Parse reject `dlc > 8` when `fd == 0` and reject `fd` for peripherals whose element size is 8 B. Or set element sizes (and `MessageRAMOffset`) per bus in `can_configure`. Clamp RX `len` to 8 when `hdr.FDFormat == FDCAN_CLASSIC_CAN`.

## MED

4. **HC-4 MED - stale flag can glitch to 1 on a fresh channel (unsigned time wrap)** (Common/Src/head_decode.c:124, CM4/Core/Src/head_cm4.c:279,315)
   - Defect: `HeadCm4_Poll` samples `now` once at the top. The RX ISR can store a newer `last_rx_ms` (newer `HAL_GetTick()`) before `HeadDecode_UpdateStale(..., now)` runs. Then `(uint32_t)(now - last)` is about 4.29e9, which is greater than the timeout, so `stale = 1`.
   - Scenario: a frame lands during the TX poll or other work after `now` is captured, once a ms tick boundary has passed. The channel is reported stale for up to 10 ms (the next UpdateStale). The window is tens to hundreds of microseconds per 10 ms pass, but it happens regularly on busy buses. CM7/dash/app see stale flicker.
   - Fix: use `(int32_t)(now - last) > (int32_t)timeout` with a negative difference counted as fresh, or re-read `HAL_GetTick()` inside UpdateStale.

5. **HC-5 MED (unverified on hardware) - CM4 can read the FDCAN kernel clock before the CM7 has configured it** (CM4/Core/Src/main.c, CM7 main.c:40 `//#define DUAL_CORE_BOOT_SYNC_SEQUENCE` commented out in both; head_cm4.c:93,226)
   - Defect: with boot sync disabled, nothing orders the CM4 after `SystemClock_Config()` on the CM7, which sets FDCAN to PLL2. `HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FDCAN)` is read once in `HeadCm4_Init` and again in `can_configure`.
   - Scenario: if the CM4 is released at reset (BCM4 option byte) it can reach `HeadCm4_Init` before PLL2 is up. `kernel_hz` comes back 0 or wrong, `Hcan_ComputeTiming` fails, and both flash and default apply return HERR_INVALID_CONFIG. The buses stay stopped silently, with only `fdcan_kernel_hz` in INFO as a hint. Unverified: it depends on the BCM4/BCM7 option bytes and on whether the CM4 is held in reset by the CM7.
   - Fix: re-enable the HSEM boot sync, or have the CM4 wait for a "clocks ready" flag and retry apply from `HeadCm4_Poll` when `can_configure` fails because the clock is 0.

6. **HC-6 MED - hub routes can read past the received frame length** (Common/Src/head_decode.c:42-48, head_cfg.c:155,159)
   - Defect: `process_hub` only checks `len < f->min_len`, then reads `data[rt->offset + b]`. Parse checks `offset + length <= 64` and `min_len <= 64` independently but never `offset + length <= min_len`.
   - Scenario: a blob (the CRC only detects corruption, it does not authenticate) has `min_len=8` and a route at offset 12. A short frame passes the check and the route decodes bytes 12-15 of the previous frame's stack contents. This is wrong data stamped as fresh (`last_rx_ms` updated). In the ISR `data[64]` is not cleared between frames. The generator sets `min_len = payload_bytes`, so it is consistent today.
   - Fix: in the Parse cross-check require `offset + length <= min_len` for every route of the frame, or bound-check against `len` in `process_hub`.

7. **HC-7 MED - unbounded work in the RX interrupt** (CM4/Core/Src/head_cm4.c:247-274, Common/Src/head_decode.c:19-34, 40-63, 71-97)
   - Defect: every frame scans up to 64 hub frames or 64 rx frames linearly. Each matching rx frame can decode up to 256 signals, each through a bit-by-bit `ExtractBits` loop of up to 32 iterations. The sniff path also copies up to 64 bytes. All of this runs in the FDCAN IRQ at priority 0, in a `while (fill level)` loop, with a float multiply-add per signal.
   - Scenario: roughly 64x4x32 bit-iterations, about 10^4 to 10^5 cycles per frame, for a frame matching a large custom rx frame. At 1 Mbit/s with short frames (up to about 8000 fps) this can starve the main loop (TX scheduler, stale, mailbox) and still fill the 10-element FIFO. Not measured: unverified numerically, but the structure is confirmed.
   - Fix: have the ISR only copy the frame into a small ring and decode in the main loop (or a lower-priority IRQ). Alternatively add a per-ID index (sorted table) and byte-wise extraction for byte-aligned signals. At least add a cycle-count/high-water stat.

8. **HC-8 MED - TX payload padding leaks uninitialised stack bytes and the DLC is not validated** (Common/Src/head_encode.c:74-76, head_cfg.c:172, CM4/Core/Src/head_cm4.c:45-52,66)
   - Defect: Parse accepts any dlc 1..64. For a dlc not in the CAN-FD set (9-11, 13-15, 17-19, ...) `len_to_dlc()` rounds up (9 -> 12), but `HeadTx_Build` only zeroes `dlc` bytes of the uninitialised `uint8_t data[64]` in `HeadTx_Poll`. HAL then transmits 12 bytes (it reads 4-byte words from the buffer), so 3 bytes of stack garbage go on the wire. For a classic bus dlc > 8 is wrong too (see HC-3).
   - Fix: reject dlc not in {1..8,12,16,20,24,32,48,64} (and >8 on classic buses) in Parse. Alternatively zero the whole 64-byte buffer in Poll and pass the rounded length.

9. **HC-9 MED - config publish to the CM7 is not atomic** (CM4/Core/Src/head_cm4.c:164-173, 203)
   - Defect: `publish_active` memcpy's up to 16 KB into `SH->active_blob`, then updates `active_len`, hash, `bus_count`, `channel_count` and `active_source` with no ordering or version/seqlock. The CM7 (CFG_READ/INFO/LIVE_GET) can run in between.
   - Scenario: the app issues CFG_READ of the active blob while a new config is applying. It gets new bytes with the old `active_len`, or the old hash with the new blob, so the readback or hash compare is wrong. `bus_count` and `channel_count` can also disagree with `live`, which `HeadDecode_Reset` has already reset.
   - Fix: add a `generation` counter (odd while updating, with `__DMB`) the CM7 checks, or have the CM7 refuse reads while `cmd_req != cmd_ack`.

10. **HC-10 MED - Parse accepts configs that cannot be applied, and the failed apply is destructive** (Common/Src/head_cfg.c:147,159-181, CM4/Core/Src/head_cm4.c:101-102,114,191-202)
    - Defect: Parse allows `peripheral` 3, two buses on the same peripheral, any non-zero bitrate and any filter id/mask. `can_configure` then fails (periph_handle NULL, timing, `nstd > 128`, HAL_ConfigFilter assert/error).
    - Scenario: the app uploads such a blob. The apply has already stopped both buses, wiped `live` and `stats` (`HeadDecode_Reset`) and zeroed the TX schedule. The rollback calls `can_configure(prev)` and its return value is ignored (line 200). If that also fails the buses stay dead while `SH->active_*` still claims `prev` is active. There is a bus outage for a bad config that could have been rejected before touching hardware. Two buses on one peripheral also overwrite `s_bus_of` so one bus index is lost.
    - Fix: validate peripheral (1..2, unique), filter id ranges and per-bus filter counts in Parse (or in a pre-check using `Hcan_ComputeTiming` before `stop_all_buses`). Report or retry if the rollback fails.

11. **HC-11 MED - bit-timing overflow with untrusted bitrate** (Common/Src/hcan_timing.c:11-13)
    - Defect: `presc * bitrate` is uint32 and can wrap. Parse only requires `bitrate != 0`.
    - Scenario: `bitrate = 0x80000000 + 5000000`, presc = 2 wraps to 10 000 000; 80 MHz % 10 MHz == 0, so a timing is "found" for a rate about 400x different from the one requested, with no error. With `presc * bitrate == 0` (e.g. bitrate = 2^23, presc = 512) `kernel_hz % 0` is a divide-by-zero (UDIV returns 0 on Cortex-M by default, but it is SIGFPE on the host test build).
    - Fix: do the multiplication in uint64_t, or reject `bitrate > kernel_hz/4` up front. Also cap `bitrate`/`data_bitrate` in Parse (e.g. <= 1 Mbit nominal, <= 8 Mbit data).

12. **HC-12 MED - HframeRx can lock out all following binary frames after one lost delimiter or overflow** (Common/Src/hframe.c:99-113,122-124,134-139)
    - Defect: in binary mode (2) a 0x00 with `n == 0` sets mode 0 instead of staying in mode 2, and a text-mode 0x00 switches to mode 2 only to start, not to finish, a frame.
    - Scenario: one byte lost or a framing error so that a frame's closing delimiter is missed (or an oversize frame that hits `n == sizeof buf`, line 111). The stream falls into text mode. The garbage then ends with a 0x00, which puts the parser in mode 2 with n=0. The next frame's leading 0x00 is consumed as "empty frame" and sets mode 0, its body bytes are parsed as text, and its trailing 0x00 re-enters mode 2 with n=0. Every subsequent request (about 6-20 bytes, never reaching 127) is dropped. It recovers only when a body byte happens to equal 0x0A/0x0D or a 127-byte text overflow occurs. Confirmed by tracing the state machine; the host app would see repeated timeouts.
    - Fix: treat 0x00 in mode 2 with `n == 0` as "stay in mode 2" (repeated delimiters), and send frames with a leading delimiter only after idle. A test: feed `00 <garbage> 00 00 <valid> 00`.

## LOW

13. **HC-13 LOW - TX encoding uses `double` on a single-precision FPU (Cortex-M4 fpv4-sp-d16)** (Common/Src/head_encode.c:44-55)
    - Defect: the +/-0.5, clamp and int64 conversion run through soft-float double routines (`__aeabi_dadd`, `d2lz`, ...) for every signal on every TX period, in the main loop.
    - Scenario: 128 signals at 1 kHz: about 100 cycles per double op, a few ops per signal; it slows the main loop and the TX scheduler jitter grows when many frames are due in one pass. A rounding mode difference also exists between host (x86 double) and M4 only if the soft-double library differs; both are IEEE so results match, unverified.
    - Fix: use `lroundf`/float with saturation by explicit comparisons on 64-bit integers.

14. **HC-14 LOW - 32-bit channels lose precision when sent** (head_encode.c:21-30,38)
    - Defect: U32/I32 channel values go through `float` (24-bit mantissa) before scaling.
    - Scenario: a u32 counter or I32 value above 2^24 is quantised (e.g. 16777217 sends 16777216). Probably fine for ADC counts but not for a timestamp-like channel.
    - Fix: for multiplier == 1 and offset == 0, use an integer path (or document the limit).

15. **HC-15 LOW - TX retry and burst behaviour** (Common/Src/head_encode.c:65-84)
    - Defect: on a failed send the frame's schedule is reset to `now + 1` (phase lost); the frame is rebuilt each millisecond with double math. At startup all frames are due at the same ms, so with more than 10 frames some are dropped into `tx_dropped` before retry. With `AutoRetransmission = ENABLE` and no ACK on the bus, the 10-element FIFO fills with frames that retry forever, and every later frame is counted as dropped.
    - Fix: keep `next_ms` phase on failure (`+= period` only on success, but do not move earlier than `now`), stagger the initial `next_ms[i]` by index, and only count a drop when the frame is actually skipped.

16. **HC-16 LOW - classic-CAN DLC > 8 on RX is not clamped** (CM4/Core/Src/head_cm4.c:39-43,252)
    - Defect: `dlc_to_len` applies the FD table to every frame; on a classic frame DLC 9-15 means 8 bytes. See HC-3 scenario B for FDCAN2; it also decodes stale bytes for FDCAN1.
    - Fix: if `hdr.FDFormat == FDCAN_CLASSIC_CAN`, `len = min(len, 8)`.

17. **HC-17 LOW - RX message-lost counter undercounts, and frames on an unmapped bus are silently drained** (head_cm4.c:244-246,250)
    - Defect: one `MESSAGE_LOST` interrupt may represent many lost frames but increments `fifo_lost` by 1. `bus == NO_BUS` frames are dropped without any counter.
    - Fix: read `FDCAN_RXF0S.RF0L`/error counters or count per interrupt as documented ("events"). Add a counter for no-bus frames.

18. **HC-18 LOW - TDC offset unchecked and HAL results ignored** (head_cm4.c:151-153,77,84-85,117)
    - Defect: `Init.DataPrescaler * DataTimeSeg1` can exceed 127 (HAL asserts only with full_assert, otherwise it overflows into the TDCF field). Return codes of `ConfigTxDelayCompensation`, `EnableTxDelayCompensation`, `ConfigGlobalFilter`, `Stop`, `DeactivateNotification` are all cast to `(void)`.
    - Scenario: a data rate with a low prescaler 4 and seg1 40 gives offset 160, silently corrupting TDCR, and TX at 5 Mbit has bit errors.
    - Fix: clamp or reject when the offset > 127 (use SSP = seg1 in the TDC window), and propagate failures.

19. **HC-19 LOW - config values not validated** (Common/Src/head_cfg.c:90-139,178)
    - Defect: `channel.value_type` > 2 (silently treated as U32 in encode), `HeadChannel_t.source`, NaN/Inf `multiplier` (the tx check `!(m != 0)` accepts NaN and Inf; rx multiplier/offset not checked at all), `id_base` bits outside `id_mask` (a hub frame that never matches), `tx can_id` beyond 11/29 bits, rx `can_id`.
    - Fix: add to the cross-checks (cheap, all in one place) so a bad project fails at upload.

20. **HC-20 LOW - NEVER_RX sentinel collides with real tick value; timeout 0 semantics** (head_decode.h:7, head_decode.c:124)
    - Defect: `HEAD_NEVER_RX = 0xFFFFFFFF` is also a valid `HAL_GetTick()` value (once per 49.7 days for 1 ms). `timeout_ms = 0` marks a channel stale after 1 ms (`now - last > 0`) with no special case.
    - Fix: keep a separate "ever received" bit/flag array, and document `timeout_ms == 0` (no timeout? always stale?).

21. **HC-21 LOW - init order exposes stale `magic` after a soft reset** (CM4/Core/Src/head_cm4.c:221-234)
    - Defect: `HeadCm4_Init` zeroes SRAM4 including `magic` first, but SRAM4 survives a system reset. If the CM7 reaches `shared_ready()` before the CM4 has run the memset (CM7 starts earlier, or the CM4 is delayed), it sees the previous boot's magic and writes `cmd_req`, `sniff_mask`, and `stage`, which the CM4 then wipes (the memset also races with CM7 stores).
    - Fix: have the CM7 clear `magic` before releasing the CM4 or at start-up; the CM4 should clear only magic at first, then initialise the rest, then set magic (the current code does set it last, good).

22. **HC-22 LOW - hub_lost accounting is unverified and can over-count** (head_decode.c:54-61)
    - Defect: the counter is taken from ID bits 15..8 and `hub_lost += (counter - last - 1) & 0xFF`, only for the first hub frame matching each received frame, with one `last` per hub (not per frame ID). A repeated counter (counter == last, e.g. several frame IDs sent per cycle with the same counter, or a retransmit) adds 255.
    - Scenario: unverified, because the hub spec for counter increment per frame is not in the files read; if a hub sends N frames per counter step the stat is garbage.
    - Fix: track `last` per (hub, frame id) or document that the counter increments per frame; treat `delta == 0` as duplicate, not 255 lost.

23. **HC-23 LOW - Hproto response capacity check is too weak** (Common/Src/hproto.c:47,141-148,159-164)
    - Defect: `cap < 64` is the only check, but CFG_READ can write up to 2+8+232 and LIVE_GET 2+4+240 = 246 bytes. The CM7 uses a 256-byte buffer today (HFRAME_MAX_BODY), so this is latent.
    - Fix: derive the max reply from `cap` (clamp `max` to `cap - 10`; LIVE_GET count to `(cap - 6)/5`), or `_Static_assert` the minimum.

24. **HC-24 LOW - blob CRC computed twice and bitwise** (head_cfg.c:16-25,48-58,64; head_cm4.c:211-213; hproto.c:123)
    - Defect: apply_flash runs Verify and then Parse runs Verify again; CRC is a bit loop (8 iterations/byte, about 130k loop iterations per 16 KB) plus a CRC32 in hproto commit.
    - Impact: a few ms of main-loop stall on the CM4 (no TX, no stale updates, no mailbox) while parsing; CAN RX still works through the ISR but with the config stopped (buses stopped in apply).
    - Fix: table-driven CRC, or call Parse only.

25. **HC-25 LOW - flash reads of the config sector at boot assume clean flash** (head_cm4.c:207-214)
    - Defect: the CM4 reads `HEAD_STORE_ADDR` directly. On STM32H7, a word with an ECC error (power loss during programming) raises a double-ECC fault/NMI on read; here there is no handler or pre-check. Unverified on hardware (hardware behaviour), plausible after an interrupted store. Also reading bank 1 while the CM7 erases/programs it stalls the CM4 (the CM4 runs from bank 2 at 0x08100000).
    - Fix: on the CM7 side hand the CM4 a verified copy, or check the FLASH ECC status flags, and never read while the CM7 holds the flash busy.

26. **HC-26 LOW - magic numbers, duplication, misleading comments** (various)
    - Sample points 800/750 permille (head_cm4.c:103,105), 128/64 filter limits (114), poll periods 10/100 ms (313,317), `hub_lost[8]`/`hub_last_counter[8]` vs `f->hub > 7` (head_decode.h:29-30, head_cfg.c:155), `8u * 64u` (head_cfg.c:164), `s_bus_of[4]` and `peripheral 1..3`, `64`/`16`/`128` SJW (hcan_timing.c:31), the 16-entry DLC table copied twice (head_cm4.c:41,47), `rd16/rd32` copied in head_cfg.c and hproto.c, per-section limits mirrored in `canharness/blob.py` (LIMITS), `HeadTx_InsertBits`/`HeadDecode_ExtractBits` duplicate the Motorola traversal.
    - Misleading: head_cm4.c:28 says `g_cfg` is "swapped with interrupts disabled" but it is also read by the main loop without a barrier; `head_encode.c:65` says "spread nothing", which reads like a TODO. `head_shared.h:4` says the CM4 "initialises it (memset + magic last)" but nothing prevents the stale-magic case (HC-21). `HeadCm4_Poll` comment in head_cm4.h says "bus status" but bus-off is not acted on (HC-1).
    - Fix: named constants, a shared `dlc_table`, a common little-endian read helper.

27. **HC-27 LOW - hub frames: only extended IDs and filter coverage** (head_decode.c:108, head_cm4.c:76,150)
    - Defect: `handled = extended && process_hub(...)`, so a standard-ID frame on the hub bus is dropped without any config option. With no filters configured on a bus and sniff off, the global filter is REJECT, so every frame on it is dropped; there is no cross-check that rx/hub ID ranges are covered by `filters[]`.
    - Scenario: a project with routes but a missing filter entry receives nothing, and `rx_dropped` stays 0 because the frames are rejected in hardware.
    - Fix: have Parse (or the generator) derive or check filters from the hub/rx frame tables, or document it.

28. **HC-28 LOW - rx decode does not check frame length for custom frames** (head_decode.c:67-97)
    - Defect: `process_rx` decodes signals from bits outside `len` as 0 (ExtractBits returns 0) and still refreshes `last_rx_ms`. A short frame (e.g. DLC 4 where 8 expected) updates every signal with zeros.
    - Fix: skip signals whose last bit is >= len*8 (or the whole frame if `len < dlc`).

## Host test harnesses (Master Configuration/tests/c)

- **T-1:** none of the tests compile `CM4/Core/Src/head_cm4.c`, so `len_to_dlc`, `dlc_to_len`, `tx_send`, `can_configure`, the mailbox and rollback logic have no coverage. HC-1, HC-2, HC-3, HC-8, HC-10 and HC-16 live there.
- **T-2:** `decode_sim.c` calls `HeadDecode_UpdateStale(&cfg,&live,now)` straight after each `Process` with the same `now`, so it cannot show the ISR/main time race (HC-4). It also reads at most 199 hex chars into `d[64]` (up to 99 bytes), so a long input overflows the harness buffer; the firmware caps at 64.
- **T-3:** `decode_sim.c` and `encode_sim.c` use a `uint8_t d[64]` that is uninitialised/unchanged between frames, like the ISR, but they never feed a frame shorter than `min_len` with a route past it, so HC-6 is untested. `encode_sim`'s `send()` never checks the DLC against the FD length set (HC-8) or classic limit.
- **T-4:** `timing_sim.c` only prints results for sensible rates; no test for `presc*bitrate` overflow (HC-11) or sample_permille > 1000.
- **T-5:** `hub_sim.c` tests the hub's `hub_proto.c`, not `Hproto`/`HframeRx` with garbage input; `proto_sim.c` drives `Hproto` with a 256-byte reply buffer, which hides HC-23. No test feeds `HframeRx` a lost-delimiter stream (HC-12).
- **T-6:** the host build uses x86 double/FMA behaviour; the CM4 build with `-mfpu=fpv4-sp-d16` may contract `v*m+o` into a fused multiply-add. Last-bit differences between host tests and firmware are possible (unverified, flags not checked).

## Checked and found fine

- Section parsing bounds (`head_cfg.c`): `off + 8 > len`, `count > (len - off) / esize`, `esize >= MIN_ELEM[id]` checks match every read offset (verified each section's last field offset against the minimum size). `esize * count` cannot overflow after the division check. The `nsec > 64` early return leaves `*out` zeroed with `hub_bus = -1`.
- Parse cross-references: every slot, route range, signal range, tx signal range and bus index is bounds-checked, so decode and encode never need to re-check indices. The `memset` on the fail path is complete.
- `HeadDecode_ExtractBits` / `HeadTx_InsertBits`: bit index guarded by `len`; shifts are at most 31; `uint16_t pos` arithmetic cannot walk out of bounds even with start_bit up to 255. Signed extension handles 32-bit fields without UB (`~0u << 32` is never executed).
- Hub route decode: `rt->length` is 1..4 and the sign-extend shift is only used for length < 4.
- `HeadTx_Build`: `memset` of `dlc <= 64` into a 64-byte buffer; saturation and NaN handling are correct; the 1ull shift is <= 31.
- `HeadTx_Poll`: signed difference compare handles tick wrap; resync after a stall avoids bursts. `stats->bus[f->bus]` is bounded by the Parse bus check.
- COBS encode/decode and `Hframe_Encode`: buffer sizes are consistent (260 = 256 + 1 + 3), all write paths check `cap`; `HframeRx` never overflows `buf` (text limit 127, binary limit `sizeof buf`).
- Hproto CHUNK: `off == stage_received` and `cnt <= total - received` prevent overflow of the stage buffer; BEGIN checks `total` against `stage_cap` and `HEAD_MAX_BLOB`.
- `g_cfg` swap: the ISR reads the pointer once per interrupt and the swap is done with IRQs disabled; the double-buffered `s_cfg[2]` means the ISR never sees a half-parsed table (the stop_all_buses before the swap also prevents HAL re-entry). The mailbox uses `__DMB()` on both sides of `cmd_req`/`cmd_ack`; the CM7 side does too (head_link.c:131-140). The CM7 command timeout (1500 ms) is longer than a normal apply.
- Sniff ring: single producer (ISR) and single consumer (CM7) with `__DMB()` before publishing `head`; a full ring drops and counts; `memcpy` length is at most 64 = `data[64]`.
- HAL usage: `HAL_FDCAN_DeInit`/`Init` with the MspInit clock refcount works for two instances. FDCAN1 RAM (at most 616 words) fits below FDCAN2's offset 1280, and FDCAN2 plus its filters stays below the 2560-word end. `HAL_FDCAN_Init` flushes only the instance's own area, so re-initialising one peripheral does not clear the other's message RAM.
- `HeadCm4_OnRxFifo0`: draining until fill level is 0 matches HAL's clear-flags-then-callback order. `dlc_to_len` indexes `(dl >> 16) & 0xF`, so no out-of-range lookup.
- SRAM4 layout: `_Static_assert(sizeof(HeadShared_t) <= 0x10000)`; no CM7 linker section is placed in RAM_D3; CM4 code is in bank 2 so it is not blocked by CM7 flash writes to the config sector except when reading it (HC-25). The comment that SRAM4 needs no clock gate is consistent with the device header (no `SRAM4EN` define); not checked against the reference manual.
- FPU: `SCB->CPACR` is enabled in SystemInit for the CM4 (system file line 184), so float use in the ISR is safe.
