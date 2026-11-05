# CM7 / common protocol review (read-only)

Scope: CM7 head_link.c, head_store.c, head_debug.c, tcan.c (+headers), CM7 main.c user code, Common hproto/hframe/head_shared.
Read for context: head_cfg.c (Verify/Parse), CM4 head_cm4.c (mailbox + init), CM7 stm32h7xx_it.c (NMI), linker script.
All line numbers refer to the files as of this review.

## HIGH

### H7-1  HIGH  Common/Src/hframe.c:99-107 (and 132-139)
Defect: binary-mode state machine returns to idle on every 0x00, even when the frame is empty (n==0), so after a single desync every later frame is misparsed as ASCII text, permanently.
Scenario: a frame is truncated (UART overrun/dropped byte, host aborted mid-frame, byte dropped by ring overflow, noise) -> parser stays in mode 2 with n>0. The next frame's leading 0x00 is consumed as the terminator of the garbage (CRC fails, silently dropped), mode=0. The real frame's body bytes now enter mode 1 (text); its trailing 0x00 sets mode 2 with n=0. The following frame's leading 0x00 hits `case 2, b==0, n==0` -> mode 0 again, body -> text, trailing 0 -> mode 2 ... The phase never self-corrects (COBS bodies contain no 0x00; a 0x0A/0x0D inside the body only fires on_line with garbage and does not change the phase). Every request gets no reply until the host happens to send a spare 0x00. Nothing times out; there is no inter-byte timeout either, so a half frame waits forever.
Fix: in mode 2, a 0x00 with n==0 should stay in mode 2 (it is the start delimiter of the next frame / an idle filler); additionally add a rx idle timeout (e.g. reset mode after ~50 ms silence, driven from HeadLink_Poll) and count/report CRC/overflow errors.

### H7-2  HIGH  CM7/Core/Src/head_store.c:54-76 (+ head_link.c:204-206, 164)
Defect: single-copy storage that is erased before it is rewritten, with no A/B slot and no protection against reading half-programmed flash.
Scenario 1 (confirmed): CFG_COMMIT with STORE (or CFG_CLEAR) erases the only copy (1 s+ for a 128 KB H7 sector, datasheet max several seconds), then programs 32-byte words in order. Power loss/brown-out/reset anywhere in that window leaves no valid config. The CRC32 over the whole blob does catch a torn write (tail stays 0xFF), so it is never mistaken for valid, but the car silently falls back to the compiled-in default blob (CM4 apply_flash fails -> apply_default) and the user's tuned config is lost. Stored image is the only copy, so the uploaded RAM config is also lost on the same reset.
Scenario 2 (unverified, needs a hardware test): an interrupted erase or program can leave cells with a bad ECC syndrome. On STM32H7 reading such a 256-bit word raises a double-ECC error (FLASH_SR DBECCERR) which is delivered as NMI. NMI_Handler (stm32h7xx_it.c:69-77, GENERATED file but user-edited) is `while(1){}`. HeadStore_Valid() reads p[8..11] and then CRCs the whole blob on every INFO (head_link.c:164) and at boot on the CM4 (apply_flash) -> a torn sector could hang the CM7 (and the CM4) at every boot/INFO, i.e. only a mass erase recovers.
Fix: use two 128 KB slots (or two flash words of header + generation counter) so the old config stays valid until the new one is verified; erase the target slot first, program the body, program a "valid" marker word last. Handle DBECC (enable FLASH_IT_DBECCERR, test FLASH_SR before trusting the sector, or keep a separate validity record in a different sector). Skip the whole write when the new hash equals the stored hash (also saves wear).

### H7-3  HIGH  CM7/Core/Src/tcan.c:15-48, main.c:176-181
Defect: TCAN2410Init() leaves the SBC unconfigured on any early failure and main() ignores its return value; nothing retries or reports; the watchdog is turned off "for debugging" permanently.
Scenario: the first SPI transaction after power-up fails/gets garbage (the chip may not be ready after only HAL_Delay(2), return values of the WD_CONFIG/INT_* writes at 18, 24-26, 39 are ignored, the chip ID read at 21 is never checked), or the scratch-pad compare fails -> function returns false at 30-36 before `SBC_CONFIG` is written at 45. main.c:179 discards the result, so the firmware runs normally while the SBC is in its start-up state and, per the chip behaviour you describe, cuts power later with no indication in INFO/debug. Conversely when init succeeds the watchdog stays disabled (WD_DISABLED, comment "for debugging"), so a hung CM7 is never recovered by the SBC.
Fix: check every return, verify DEVICE_ID against the expected value, retry the init sequence a few times with a delay, report the result (flag in INFO/stats and an ASCII/err line), and make the watchdog setting a deliberate build-time choice (or implement the periodic trigger via WD_INPUT_TRIG). Always attempt the SBC_CONFIG write even if the scratchpad test fails. (Register bit meanings of SBC_STARTUP_CFG / WD_DISABLED were not checked against the datasheet: tcan2410-q1.pdf could not be read here (no pdftotext) -> unverified.)

## MED

### H7-4  MED  CM7/Core/Src/head_link.c:124-141 (with CM4 head_cm4.c:286-298)
Defect: after a mailbox timeout the request stays pending; the CM4 executes it later against whatever is in `stage`/`cmd_len` at that moment.
Scenario: CM4 busy/late -> CM7 returns HERR_TIMEOUT for COMMIT(APPLY) after 1.5 s but cmd_req != cmd_ack stays set. The host re-uploads (BEGIN/CHUNK overwrite `stage`) while the CM4 finally runs APPLY_STAGED with the old cmd_len on a half-overwritten stage (fails validation, spurious error) or, if the second upload has completed, applies a config the host has not yet committed. Also a new request issued while the CM4 is mid-processing the old one is acked with `cmd_ack = cmd_req` (new value) carrying the old result (CM4 side, head_cm4.c:297). The write of cmd/cmd_arg/cmd_len (128-130) races with a late CM4 reading them.
Fix: on timeout, treat the mailbox as busy and refuse new uploads/commits until cmd_ack catches up (HERR_BUSY), or let the CM4 copy the stage under a seq/length check; do not reuse the stage while a request is outstanding.

### H7-5  MED  CM7/Core/Src/head_link.c:119-122, 321 ; CM4 head_cm4.c:224-233
Defect: SRAM4 survives a warm/system reset, so `SH->magic` from the previous run is still valid while the CM4 is re-booting, and the CM4 memsets the whole struct (including `stage`) after the CM7 may already be using it.
Scenario A: HMSG_REBOOT / `reset` -> system reset; CM7 boots first (no HSEM sync, DUAL_CORE_BOOT_SYNC_SEQUENCE is commented out), shared_ready() is true from stale magic; the host's first INFO reports stale hash/source/channel_count and cm4_alive=true (stale heartbeat differs from 0, op_info:150-156); a COMMIT(APPLY) in that window writes cmd_req/cmd to memory the CM4 then zeroes -> 1.5 s timeout, or the command is applied to a CM4 that has not finished init. Scenario B: the upload (CHUNK memcpy into `stage`, hproto.c:113) is not gated on shared_ready(), so a CM4 that boots late wipes a partly uploaded stage; COMMIT then fails with a CRC error (safe but confusing).
Fix: CM4 clears the magic as its very first action (or CM7 clears it before releasing/reset), CM7 additionally waits for a "boot counter" change; gate BEGIN/CHUNK on shared_ready(); declare `magic` volatile in head_shared.h:38.

### H7-6  MED  Common/Src/hproto.c:108-110
Defect: CFG_CHUNK is not idempotent: a retransmit of the last (already acked) chunk is rejected with HERR_RANGE.
Scenario: the reply to chunk N is lost (UART loss, the framing bug H7-1, ring overflow); the host retries chunk N with the same offset; device has stage_received = off+cnt so `off != stage_received` -> RANGE. The host can only restart with BEGIN. With a 1 s+ link timeout and a 16 KB blob that is ~70 chunks.
Fix: accept a chunk whose offset+length equals the previous chunk (compare the bytes or just re-ack with the current stage_received); keep RANGE for everything else.

### H7-7  MED  CM7/Core/Src/head_link.c:169-191, 218-225
Defect: partial-success reporting and ownership: COMMIT(APPLY|STORE) applies to the CM4 first and only then writes flash; if the flash write fails the host gets an error although the new config is now active; and the CM7 writes `SH->active_source` (188), which the header documents as CM4-owned.
Scenario: flash erase/program error -> reply HERR_FLASH, but CM4 runs the new config (active_source=2, hash changed), applied/stored flags are lost because the error response replaces them. CFG_CLEAR without REVERT erases flash but leaves active_source=1 ("flash") while the sector is blank (INFO then claims the active config comes from flash). The write at 188 can race a CM4 update of the same field.
Fix: write flash first, then apply (or apply, then on store failure still return success with applied=1/stored=0); have the CM4 set active_source via the command argument instead of the CM7 poking it; make CLEAR update the source (e.g. send APPLY_DEFAULT or a new "source changed" command).

### H7-8  MED  head_store.c:36-52, head_link.c:99-102/135-138, tcan.c:59/90
Defect: long blocking sections on the single main loop / ISR-less peripherals.
- Flash erase/program (comment says ~1 s; H7 128 KB sector erase can be longer): the CM7 fetches code from the same bank, so it stalls entirely: UART4 RX ISR cannot run (8-byte FIFO overruns -> bytes lost -> H7-1), SysTick misses ~all ticks during the stall (HAL_GetTick/uptime lose that time, op_info cm4_alive and the 1.5 s mailbox timeout compare against a skewed clock), sniff streaming stops. Relocating the erase code to RAM/ITCM does not help for same-bank reads, so the host must be told to expect it (document a COMMIT reply latency, or reply "busy" first).
- cm4_command() spins up to 1.5 s in main context (drains nothing; the 1 KB ring is ~90 ms of data at 115200 baud, overflow drops new bytes silently, head_link.c:61).
- uart_send() blocks up to 250 ms per frame, ignores the return value (partial frames are sent silently, corrupting the stream for the host); sniff streaming does this 2x per loop.
- TCANSpiRead/Write use HAL_MAX_DELAY: a stuck SPI hangs the CM7 forever with CS asserted.
Fix: bounded timeouts everywhere with error reporting; count ring overflows (and report them in STATS); have the host treat COMMIT as slow; use a finite SPI timeout and abort/recover.

### H7-9  MED  Common/Src/hframe.c:99-131 + head_link.c:331-335
Defect: binary frames can be dispatched as ASCII commands and the RX path has no error accounting.
Scenario: any bytes that fall through to text mode (see H7-1, or line noise on an idle line) are executed by on_line as debug commands (`reset`, `send:tcan:..`). Probability for garbage to spell a command is tiny, but `send:tcan:XXXX` takes any hex-looking string, and an unsolicited '\n' in a binary body (mode 1) triggers `on_line` with the binary garbage which gets echoed back in the "ERR_BAD_CMD" reply (head_debug.c:81, `cmd` is attacker-controlled up to 127 bytes and written into a 160 byte buffer by snprintf, truncation OK). Ring overflow, UART ORE/FE (HAL_UART_ErrorCallback just re-arms, head_link.c:68-72) and CRC failures (hframe.c:103) are all dropped with no counter.
Fix: add error counters (rx_overrun, crc_fail, framing) to the stats and gate the ASCII interface (build flag or only before the first valid binary frame).

### H7-10  MED  CM7/Core/Src/head_debug.c:45-55
Defect: `send:tcan:<reg><val>` is an unrestricted raw write to any TCAN register from a console that is also live in the field.
Scenario: `send:tcan:0C00` or a typo writes SBC_CONFIG/WD_CONFIG/INT_EN registers (sleep mode, watchdog on, power rails) which can cut the module's power; no unlock/confirmation. Invalid hex digits are silently treated as 0 (hex_val, 20) and extra digits are silently ignored (hex_to_bytes stops at max=2), so `send:tcan:0Cxx` = write 0x00 to 0x0C. The register is the raw SPI address byte, not the TCanReg_t number, which is not stated anywhere.
Fix: validate hex digits (reject on error), exact length 4, restrict to a whitelist of registers or compile it out in release builds.

## LOW

### H7-11  LOW  CM7/Core/Src/head_link.c:338-350
Defect: the sniff entry length and the stats bus count come from shared memory without clamping.
Scenario: e->len > 64 (corrupt/stale ring from the other core) -> `memcpy(p+11, e->data, e->len)` overruns `p[75]` on the stack; SH->bus_count > HEAD_MAX_BUSES (stale/corrupt SRAM4 before the CM4 init, see H7-5) -> op_stats_get writes 18*nb bytes into the 254-byte response (head_link.c:254-272, only the buffer cap is checked against the max bus count, not nb) and indexes SH->stats.bus[] out of range. Also `SH->sniff_tail` is not DMB-ordered after the entry reads before being published (349).
Fix: clamp `len` to 64 and `nb` to HEAD_MAX_BUSES; add `__DMB()` before updating sniff_tail.

### H7-12  LOW  CM7/Core/Src/head_link.c:193-216
Defect: CFG_READ source 0 copies from `SH->active_blob` while the CM4 may swap the active config between chunk requests; `active_len` and the blob are not read atomically.
Scenario: the host reads the active config in 232-byte chunks across several messages while an APPLY happens -> a Frankenstein blob with the old total length. The host can only detect it by re-checking the hash.
Fix: have the reply carry the active hash with each chunk (or reject reads when the hash changed since the first chunk).

### H7-13  LOW  CM7/Core/Src/head_link.c:75-97, 99-102 and main.c:173-181
Defect: boot window with contending UART pins and a hard-coded pin assignment. MX_UART4_Init() drives PC10 as TX push-pull into the ST-LINK's TX output until HeadLink_Init() (after HAL_Delay(2) + TCAN2410Init()) swaps the pads; the PC10/PC11/GPIOC/AF8 numbers are hard-coded in user code instead of the .ioc. Re-running HAL_UART_Init() relies on gState != RESET to skip MspInit (works, checked in HAL).
Fix: call uart_apply_pin_swap() immediately after MX_UART4_Init(); move the swap into the .ioc (ask first) once the next board revision fixes the wiring.

### H7-14  LOW  CM7/Core/Src/head_link.c:164, 305-308; hproto.c:175-177
Defect/perf: op_info recomputes HeadStore_Valid (bitwise software CRC32 over up to 16 KB of flash, ~1-2 ms, and it reads flash that can be mid-erase from the other core's view) on every INFO poll; REBOOT is unauthenticated and resets both cores without a handshake (CM4 mid-apply or CAN TX is cut; also `reset` debug command at head_debug.c:75-79); every STORE erases the sector even for an identical blob (wear).
Fix: cache stored hash/len after boot and after each write/erase; skip identical writes; require a magic argument for REBOOT.

### H7-15  LOW  Misc cleanup
- head_debug.c:13/head_debug.h:7: `sniff_tcan`, `sniff_afeA`, `sniff_afeB` are set but never read anywhere in the CM7 code (dead; the `sniff:` ASCII command does nothing). `extern uint8_t tcan_global_int` is redeclared in head_debug.c:11 instead of tcan.h.
- tcan.h has no include guard; tcan.c:14 comment "turn off the SPI" is wrong; tcan.c:40-42 empty `if`; tcan.c:20-21 unused `device_id`; TODO CRC/interrupt never implemented; `reg << 1 | 1` is in a hard-coded format with no named R/W constants; TCANSpi* share global tx/rx buffers (not reentrant, fine while single-threaded).
- Magic numbers: 250 ms TX timeout (head_link.c:101), 232 read cap and 48 live count (hproto.c:141,159), `1u+18u*...` stats size (head_link.c:251), `HAL_Delay(2)/(5)`, NVIC priority 5, 0xFFFF timeouts in unused boot-sync blocks; `HEAD_FW_VERSION` defined in head_link.c.
- head_shared.h:6 ownership comment is wrong (CM7 also writes `active_source`, head_link.c:188; CM4 zeroes sniff_mask/tail at init).
- head_store.c: `sector_error` is not looked at (status is, which is enough), the post-write `memcmp` verification would read stale data if the D-cache were enabled (header says it is off; add an invalidate or MPU note in head_store.c); `uint32_t word[8]` aligned(32) on the stack forces stack realignment.
- hproto.c:121-135: COMMIT with flags==0 (or unknown bits) succeeds as a pure validation and clears stage_active; all errors on COMMIT also clear stage_active so the host must re-upload everything.
- hframe.c:103: a body of length 2 or 3 (valid length-4+CRC rule) is accepted only for d>=4; silent drop with no error reply by design, but undocumented in protocol.md.

## Checked and found fine
- Hcobs_Encode/Decode bounds: every write is capped (`cap`), decode rejects code==0 and `i+code>n`; worst-case encoded size of a 256 B body is 258 B and fits both HFRAME_MAX_ENCODED (260) and Hframe_Encode's `cap-2`; the on-stack `body[256]` and `buf[260]` cannot overflow.
- HframeRx text mode caps at 127 chars + NUL; binary mode caps at sizeof buf; rx->n is uint16_t, no overflow.
- Hproto length checks: BEGIN requires n==8, total in [32, HEAD_MAX_BLOB]; CHUNK requires sequential offset, cnt<=240, `cnt <= total-received` (no underflow because received<=total), memcpy into stage is in range; COMMIT requires received==total and re-checks CRC32; max response sizes (INFO 50, CFG_READ 242, LIVE 246, STATS 141) fit the 256-byte resp and Hframe_Encode's 252-byte payload limit; CFG_READ offset>len rejected, n computed without overflow.
- Re-BEGIN in the middle of an upload cleanly restarts (stage_received reset); CHUNK without BEGIN -> BUSY; COMMIT only once per upload; a second COMMIT after success fails with BUSY.
- UART RX ring: single producer (ISR writes head) / single consumer (main writes tail), 16-bit volatile indices, power-of-two-independent modulo, drop-newest on full; no race found. HAL_UART_Transmit (gState) and Receive_IT (RxState) do not conflict.
- cm4_command(): __DMB() before publishing cmd_req and after seeing ack; unsigned tick subtraction handles wrap; CM4 side checks req!=ack and DMBs before reading.
- Flash geometry: sector 7 of bank 1 = 0x080E0000..0x080FFFFF, 128 KB, linker script FLASH limited to 896 K so the image cannot overlap; programming uses 32-byte aligned addresses with 0xFF padding and a source buffer aligned(32); Lock is always called on every path; erased flash reads as 0xFF so HeadStore_Valid rejects a blank sector (length 0xFFFFFFFF > max); CRC32 covers header (CRC field zeroed) and the entire body so a torn write is never accepted as valid.
- HeadStore_Valid/Parse length handling: `rd32(b+8)==len`, len<=HEAD_MAX_BLOB, section counts use division to avoid multiplication overflow (head_cfg.c:78).
- Debug parser: strtok_r with local copy and forced NUL, NULL checks present for every optional token, reply buffers are bounded by snprintf, `reset` replies before resetting (HAL_UART_Transmit waits for TC).
- tcan.c: CS is released on both success and failure of the SPI transfer, register address byte fits in 7 bits (max 0x62), status byte captured on each frame, scratch-pad write/readback test with two patterns is meaningful.
- Pin-swap: HAL_UART_Init re-run with SWAP advanced feature is valid; FIFO mode preserved via huart->FifoMode.
- op_sniff_set checks bus < bus_count (<=4) so `1u << bus` is defined; op_live_get bounds slot by channel_count (<=256).
