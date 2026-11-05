# Hub firmware review (SensorHubV1) - read-only

Root: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/SensorHubV1/Core. Paths below are relative to it.
Core/Inc/as.vhdl: DOES NOT EXIST (not in Core/Inc, and `find` finds no as.vhdl anywhere in the workspace). Nothing to review.
Context facts used: SYSCLK = HSI/5*100/2 = 160 MHz (.ioc, main.c:388-394); all NVIC priorities are 0 (SysTick 15), so ISRs do not preempt each other (ISR-vs-ISR races on the AFE flags are not possible; ISR-vs-main races are); FDCAN kernel = PLLQ = 80 MHz (500 kbit/s nominal, 5 Mbit/s data); HAL G4 FDCAN TX FIFO is a fixed 3 elements.

## HIGH

**HUB-1 HIGH** - Src/stm32g4xx_it.c (no handler), Src/main.c:737-760, SensorHubV1.ioc NVIC list (GENERATED, interacts with afe_ctrl.c:26)
Defect: DR_A (PC7) / DR_B (PC9) are configured as falling-edge EXTI inputs, but EXTI9_5_IRQn is not in the .ioc NVIC list, is not enabled in MX_GPIO_Init, and there is no EXTI9_5_IRQHandler in stm32g4xx_it.c.
Scenario: when acquisition is enabled, HAL_GPIO_EXTI_Callback is never called; no sample is ever read. If someone only enables the IRQ in NVIC without regenerating, the vector goes to Default_Handler (infinite loop = hub lockup).
Fix: enable EXTI line[9:5] interrupt in CubeMX (regenerates NVIC enable + handler calling HAL_GPIO_EXTI_IRQHandler for pins 7 and 9). Ask before touching the .ioc.

**HUB-2 HIGH** - Src/main.c:731,735 (generated GPIO init), no AFE init code anywhere; Src/afe_ctrl.c
Defect: nothing brings the AFEs up. RST_AB (PA8) is driven LOW at init and never set high (grep: only main.c:731/743), CS_A/CS_B are initialised LOW (main.c:735) so both chip selects idle asserted, no RESET/START command or START pin, no initial write of config[0] to either AFE, no check of ID register.
Scenario: AFEs stay in hardware reset (RESET pin is active low) -> no conversions, no DRDY. Even if released, the ADS124S08 would sit at defaults (no START -> no conversions in the continuous mode this code assumes), and the first sample is labelled idx 0 while the mux is still at power-on default.
Fix: add AfeInit(): CS high, RST_AB high + wait >= 4096*tCLK, read ID, write config[0] for each AFE, send START (or tie pin), then enable EXTI; keep CS idle high.

**HUB-3 HIGH** - Src/afe_ctrl.c:53, 68, 86-90, 126, 143
Defect: both call sites pass `0` as the first argument of read_data_and_write_reg(afe_is_A, ...), so CS_B is always the pin asserted. The A path of the completion callback then releases CS_A (never asserted) and leaves CS_B low.
Scenario: AFE A is never addressed; its data is never read; reading "A" actually talks to B. After one "A" cycle CS_B stays low forever and the next B transaction starts with CS already low (no CS edge -> command framing lost). The sniff TX event also reports AFE B for everything (line 99).
Fix: pass `1` in the A branch (line 68) and `0` in the B branch (53), and name the parameter/enum (AFE_A/AFE_B) instead of 0/1/"afe_is_A".

**HUB-4 HIGH (verify in ADS124S08 datasheet; not in repo)** - Src/main.c:703-709 (SPI2 init: CLKPolarity LOW, CLKPhase 1EDGE = SPI mode 0)
Defect: I recall the ADS124S08 only supports SPI mode 1 (CPOL=0, CPHA=1); SPI2 is configured as mode 0. (TCAN on SPI1 is correct: its default is mode 0.)
Scenario: every AFE byte is sampled half a bit off; reads/writes return shifted garbage, registers get wrong values.
Fix: SPI2 CLKPhase = SPI_PHASE_2EDGE (user code can set it after MX_SPI2_Init via HAL_SPI_Init in USER CODE SPI2_Init 2, or in .ioc after asking).

**HUB-5 HIGH (unverified: needs ADS124S08 datasheet / bench)** - Src/afe_ctrl.c:72-104, 109-121
Defect: the sample is taken from rx bytes [0..2] of a transaction whose first bytes on DIN are a WREG command (0x42, 0x06), not RDATA (0x12) or a bare direct read. The header comment (afe_ctrl.h:42) itself notes the RDATA/direct-read distinction. With RDATA the data comes out after the command byte (offset 1..3), and in direct-read mode DIN carries NOPs; it is not established that the device shifts conversion data out while it is being sent a WREG.
Scenario: raw_data is always 0 or garbage; LIVE_SENSOR_DATA is wrong while looking plausible.
Fix: do RDATA+3 bytes (data at rx[1..3]) as a separate frame, or confirm on a scope that DOUT carries data during WREG; also consider status byte/CRC disabled assumptions (SYS register default).

**HUB-6 HIGH** - Src/afe_ctrl.c:117-121,135-139; Src/can_scheduler.c:175-179; generated config (all AFE signals have byte_length = 2)
Defect: the stored value is the raw 24-bit two's complement code, zero-extended (no sign extension), no offset/multiplier applied (the app UI says "Sent raw = (value - offset) / multiplier", index.html:631), and the scheduler then copies only the LOW `byte_length` (2) bytes.
Scenario: CAN carries bits 15:0 of the ADC code: the MSBs are discarded, the signal wraps every 65536 counts and negative readings (differential sensors) are meaningless. The LIVE_GET table (hub_link.c:117) shows 24-bit raw, so the UI and CAN disagree.
Fix: decide where scaling lives; either scale/clip to the generated length in the scheduler (sign-extend with `(int32_t)(raw << 8) >> 8`, apply offset/multiplier) or have the generator emit byte_length 3 and take the 24-bit value. Unverified: whether the generator intends the hub to scale (generator not in scope).

**HUB-7 HIGH** - Src/tcan.c:57-106, Inc/tcan.h:37 (datasheet 9.1.10, 8.3.1, Table 8-1/8-2)
Defect: CAN_CNTRL_1 (0x10) is never written. Its reset value is 0x04 -> CAN1_TRX_SEL = 100b "Wake capable", in which the driver is Z/Z (Table 8-2: "Wake capable: CANH/CANL Z, weak pull to GND"). Only 110b (SBC mode control -> ON in normal) or 111b turns the transmitter on. tcan.h defines CAN_CNTRL_1 but nothing uses it.
Scenario: when the CAN scheduler is enabled the SBC goes to normal mode (SBC_STARTUP_CFG) but the transceiver never drives the bus; TX never gets an ACK, frames retry forever (AutoRetransmission ENABLE, main.c ~437), TX FIFO fills, tx_fail climbs.
Fix: in TCAN2410Init, after SBC_CONFIG write, write CAN_CNTRL_1 = 0x06 (or 0x07) and read back; also check VCAN_STATUS (HSS_CNTL3 bit 1).

**HUB-8 HIGH (when enabled)** - Src/main.c:295-318
Defect: in the commented-out block the order is CanScheduler_Init -> HAL_FDCAN_Start -> HAL_TIM_Base_Start_IT, all BEFORE the block that calls TCAN2410Init().
Scenario: FDCAN starts and the 200 Hz tick fires while the SBC is still in standby with the transceiver off: frames are queued/retried with no bus, errors accumulate (possibly bus-off), and the tick ISR runs during the time-critical SBC watchdog-disable window (SPI polling takes ~ms, tick ISR loads the CAN path).
Fix: call TCAN2410Init() first (it must stay the first thing for the watchdog), then CAN_CNTRL_1, then CanScheduler_Init/HAL_FDCAN_Start/TIM2 start; only start if TCAN init returned true.

**HUB-9 HIGH (when enabled)** - Src/main.c:489-492 (TIM2: Prescaler 170-1, Period 4999), 523-526 (TIM3) - GENERATED values, interacts with can_scheduler.c
Defect: the prescaler assumes a 170 MHz timer clock; APB1 timer clock is 160 MHz (SYSCLK 160, APB1 /1; .ioc APB1TimFreq 160000000). 160e6 / 170 / 5000 = 188.2 Hz, not 200 Hz. (TIM3 "1 us" tick is 1.0625 us.)
Scenario: "200 Hz" frames go out at 188 Hz, 100 Hz at 94 Hz, 10 Hz at 9.4 Hz - the receiver sees 6% off rates and the timestamp (HAL_GetTick) shows non-uniform spacing (5/5/6 ms).
Fix: Prescaler 160-1 with Period 5000-1 (=1 MHz/5000=200 Hz), or Prescaler 0 / Period 799999 (ARR is 32-bit on TIM2). Needs .ioc change (ask first).

**HUB-10 HIGH** - Src/main.c:157-237 (processDebugCommand)
Defects (all confirmed by reading):
 a) 168-169: `send:` with no 2nd token -> strtok returns NULL -> strcmp(NULL,...) hard fault (hub locks in HardFault_Handler's while(1)). Same at 172-175 (`send:tcan` with no hex -> strlen(NULL) in hexStringToBytes).
 b) 167-185: `send:fdcan`, `send:afea`, `send:afeb`, unknown targets never write resp_buffer; line 236 then does strlen()/transmit of an uninitialised 128-byte stack array (undefined length, can read past the buffer, prints stack garbage).
 c) 174-178: str_bytes is uninitialised; hexStringToBytes can return 0 or 1 bytes (odd/short hex, invalid chars become 0 silently) and the code writes str_bytes[0], str_bytes[1] to the SBC anyway -> write of a random register with a random value.
 d) 211-215 also calls TCANSpiRead 5 times from the main loop with HAL_MAX_DELAY waits (see HUB-15).
Scenario: a typo or a line like `send:tcan:0C` (or any desync garbage that happens to begin with "send") crashes the hub or reprograms the SBC (e.g. SBC_CONFIG sleep -> 3V3 cut, or watchdog config -> rail cut).
Fix: NULL-check every strtok result, initialise `resp_buffer[0]=0` / send an ERR for each branch, require hex length == 4, validate hex chars, restrict/disable `send:tcan` write to a whitelist of registers.

**HUB-11 HIGH (functional, user-visible)** - Src/debug_sniff.c:7-15, Src/main.c:42
Defect: `#define DEF_DEBUG` is inside main.c only (USER CODE PD). debug_sniff.c has `#ifndef DEF_DEBUG` at its own top, which is true in that translation unit (no -DDEF_DEBUG in the toolchain settings: .cproject/Debug makefile have no such define; Debug/SensorHubV1.list shows the `#ifndef` branch). So only the empty stubs (lines 9-13) are ever compiled; the whole queue/snprintf implementation (lines 17-88) is dead code.
Scenario: `sniff:tcan:on`, `sniff:afea:on` reply "ON" (main.c:195-201) but nothing is ever emitted; the TCAN/AFE sniffing feature silently does nothing. The sniff flags in main.c are written but never read.
Fix: put DEF_DEBUG in a shared header (main.h/ a config header) or a project-level -D, so all TUs agree.

**HUB-12 HIGH (when enabled)** - Src/afe_ctrl.c:103,107-157
Defect: the return value of HAL_SPI_TransmitReceive_DMA is ignored and there is no HAL_SPI_ErrorCallback/AbortCpltCallback or timeout. afe_spi_busy (set at 46/61) is only cleared in TxRxCpltCallback.
Scenario: any HAL_BUSY/ERROR start (e.g. SPI2 busy), an SPI overrun or DMA error, or a missed DRDY edge leaves afe_spi_busy = 1 (or the DRDY chain unrestarted) and CS low forever: acquisition stops permanently with no indication and no recovery; the LIVE table freezes with last values and no staleness flag (hub_link.c:120).
Fix: check the return, on failure deassert CS, clear busy and count an error; add HAL_SPI_ErrorCallback; add a main-loop watchdog that restarts the chain if no sample for X ms (or poll DRDY level when idle).

## MED

**HUB-13 MED** - Src/can_scheduler.c:216-240,202-206; HAL fdcan.c:216 (3 TX elements)
Defect: all groups due on a tick are queued back to back into a 3-element TX FIFO from the ISR; failures are only counted (tx_fail_count), nothing retries, no TX-complete/ FIFO-level check (HAL_FDCAN_GetTxFifoFreeLevel).
Scenario: phase 0 sends the 200 Hz + 100 Hz + 10 Hz groups at once (already 3 frames with the current config); any added group (up to 24 are allowed, line 8) or a slow/blocked bus (no ACK, bus-off, HUB-7) drops frames silently each tick; at 500 kbit/s arbitration + 5 Mbit/s data, a 64-byte frame takes ~100-150 us, so bursts are fine only while the FIFO is not already full.
Fix: spread groups across ticks (offset per group), check free level before adding, count drops per frame id.

**HUB-14 MED (unverified)** - Src/main.c:433-451 (GENERATED FDCAN init), no HAL_FDCAN_ConfigTxDelayCompensation / EnableTxDelayCompensation call anywhere in user code (grep)
Defect: data phase 5 Mbit/s (DataPrescaler 1, 16 tq at 80 MHz) with BRS ON and no transmitter delay compensation. At 5 Mbit/s the transceiver loop delay (TCAN2411 plus board) is comparable to the 200 ns bit time.
Scenario: FD frames with BRS are not decoded by the transmitter (bit errors in data phase, error counters rise) while slower bit rates work.
Fix: call HAL_FDCAN_ConfigTxDelayCompensation(&hfdcan1, offset≈ DataTimeSeg1+1 tq, 0) and HAL_FDCAN_EnableTxDelayCompensation before Start; verify against the datasheet loop-delay figure.

**HUB-15 MED** - Src/tcan.c:117,148 (HAL_MAX_DELAY), Src/main.c:133,308-310
Defect: TCANSpiWrite/Read use HAL_MAX_DELAY on polled SPI. If SPI1 clock/pins fail (or the pin is shorted) the call never returns, before the heartbeat/`HubLink_Poll` can run and while the SBC watchdog is counting. tcan_xfer_crc (188) uses 100 ms; inconsistent.
Scenario: bring-up problems show as a silent hub with no UART output (UART is only polled after TCAN init at the start of main, line 304) instead of the "tcan:init:FAIL" message.
Fix: use ~10 ms timeouts, abort on error (HAL_SPI_Abort), keep the result of TCANSpiWrite checked (lines 88-90 ignore it).

**HUB-16 MED** - Src/main.c:216-223, Src/tcan.c:207-249
Defect: TCANSaveWatchdogConfig (500-write EEPROM) is callable from the unauthenticated ASCII line `tcan:save:FORCE` at any time after init; the comment says WD config can only be programmed in standby, but by then the SBC is in normal mode (SBC_STARTUP_CFG written at tcan.c:100), so the "registers hold watchdog off" pre-check can pass while the save is made from a different state than the one it was designed for. Garbage bytes that become a valid line, or a script retrying FORCE, burn EEPROM cycles.
Scenario: unintended permanent EEPROM writes; a half-finished save with CRC enabled leaves SPI broken (CRC_STUCK) until nRST.
Fix: require standby (read SBC_CONFIG mode bits) or remove the runtime command and keep only the one-shot compile-time path; rate-limit/ count saves.
Note: the CRC procedure itself matches the datasheet (poly 0x2F, init 0xFF, xor 0xFF; checked: CRC(0x9D,0x8A)=0x36, CRC(0x15,0x00)=0x5E, equal to Table 8-25).

**HUB-17 MED** - Src/main.c:116-131 and 107-114
Defect: HAL_UARTEx_ReceiveToIdle_DMA restart return value is ignored in RxEventCallback (line 128) (ErrorCallback does count it). If it returns HAL_BUSY (RxState not READY) reception is permanently dead with no retry; also bytes arriving between the idle event and the restart are lost, and the error path discards the partially received burst.
Scenario: the hub becomes deaf (transmit still works) until reset; the main loop does not detect it (rxst is only printed).
Fix: on failure set a flag and retry from the main loop; or use circular DMA with half/full + idle events and a read pointer.

**HUB-18 MED** - Src/hub_link.c:176-189, 164-174
Defect: s_flush is cleared after being read (180-181) but the ISR can set it in between; the ring loop then consumes the new bytes and flush is lost. Also the flush from an earlier burst is applied after consuming a LATER burst's partial line (line 187), prematurely ending an ASCII command; overflow (169) returns without setting s_flush.
Scenario: a command typed without a line terminator (e.g. "ping" from a terminal) is sometimes not executed until more input arrives or executed truncated.
Fix: use an atomic exchange / critical section for s_flush, or store the "burst end" position in the ring (a sentinel) so the end of a burst is processed in order.

**HUB-19 MED** - Src/hframe.c:95-141
Defect: stream resync is lossy: after a bad/garbled binary frame (CRC failure, mode 2 aborted by 0x00), the next frame's leading 0x00 is consumed as the end delimiter of the garbage, so the following frame is parsed as ASCII text (mode 1) until its trailing 0x00, then the next frame's leading 0x00 is consumed as an empty frame. Net effect: one good frame is silently dropped after any corruption. Also a >127 char ASCII chunk is split and the tail processed as a second command (line 125-129), binary bytes 0x0A/0x0D inside mode 1 end the "line".
Scenario: the app's request after a corrupted frame gets no reply; retry works. Not a crash. (Buffer bounds are all correct: rx->buf is HFRAME_MAX_ENCODED=260 and decode cap is 256.)
Fix: in mode 2, if the frame fails CRC or n==0 stay in mode 2 (treat the 0x00 as the start of the next frame); only fall to idle on a good frame.

**HUB-20 MED** - Src/afe_ctrl.c:20-21, 123, 141, 36-70
Defect: per-sensor `sample_rate` is ignored; both AFEs round-robin all channels equally (rate per channel = ODR/N-ish). The code does not enforce a settle time itself: it relies on the generated DATARATE (0x1D = low-latency filter, 4000 SPS) and on a write to INPMUX restarting the conversion; with a sinc filter or a slower rate in another config the first sample after a mux change would be unsettled. Also each DRDY while busy merges into one flag (a DRDY missed while the SPI is busy is silently skipped).
Scenario: with the current table (8 channels at 4000 SPS) ~400 Hz per channel is fine for 200 Hz; with 10 channels at lower ODR it would be below the requested rate with no warning. Unverified how the generator checks ODR/N >= requested rate.
Fix: validate at init that datarate/N >= max requested rate (and that FILTER bit is low-latency), report in stats.

**HUB-21 MED** - Src/afe_ctrl.c (no code), Src/stm32g4xx_it.c, Src/main.c
Defect: digital inputs DIN0-3 (and FAULT0-3, PG_5V, CAN_INT) are never read: DIG_ACTIVE_SENSORS_CONFIG entries (generated, mapping slots 20,21) are put in CAN frames (can_scheduler.c:133) but nothing ever writes LIVE_SENSOR_DATA for them.
Scenario: digital sensor channels transmit 0 forever.
Fix: sample the GPIO in CanScheduler_Tick200Hz (or a 1 kHz tick) into LIVE_SENSOR_DATA according to the DIG config.

**HUB-22 MED** - Src/main.c:337-360 and 132-134
Defect: the "hub:alive" heartbeat (HUB_DIAG_HEARTBEAT_MS 2000) is compiled in and is written onto the same UART as the binary protocol, plus diagnostic polling of PA2; with HAL_UART_Transmit blocking up to 100 ms from the main loop.
Scenario: the config app receives an ASCII line every 2 s in the middle of its binary traffic (it must tolerate it); on the real harness UART the heartbeat is noise and costs main-loop time.
Fix: make it off by default / runtime switch.

## LOW

**HUB-23 LOW** - Src/debug_sniff.c (dead today, defects if enabled)
 - line 50-55: `hex[64]` with `i < sizeof(hex)/2` (32 bytes -> 64 chars + NUL = 65 bytes): 1-byte overflow if len>=32 (current callers pass <=8, latent).
 - 67-76: `__disable_irq()/__enable_irq()` unconditionally enables interrupts when called from an ISR or any masked context (should save/restore PRIMASK).
 - 79-85: flush reads sniff_queue[sniff_read] while an ISR "drop oldest" (70-72) can advance sniff_read and overwrite that slot; HAL_UART_Transmit blocks 100 ms per entry in the main loop.
 - Called twice per AFE sample from the ISR (afe_ctrl.c:99,152) with snprintf: at the AFE sample rate (kHz) this cannot work when sniffing is on.
 - `local[128]` + hex[64] + vsnprintf on ISR stack.
 - AFE sniff id is derived from `busy_afe` AFTER it may be reset (afe_ctrl.c:152) and from the hard-coded afe_is_A (HUB-3).

**HUB-24 LOW** - Src/afe_ctrl.c:106-157
 Duplicate A/B branches (copy-paste, easy to diverge); `afe_tx_buffer`/`afe_rx_buffer` single shared buffers (OK only because transactions are serialised by afe_spi_busy); `process_spi_queue` and the callback share non-atomic flags (safe now only because all IRQ priorities are equal; document it or use a critical section); `busy_afe` is reset only in the empty-config case; globals non-static; magic numbers 6/7/9 (AFE_REGS_TO_WRITE -1 passed as "num_bytes_minus_one"). afe_tx_buffer sized 9 = 2+7 exactly (no margin if a register is added).

**HUB-25 LOW** - Src/can_scheduler.c
 - 225: s_tick_phase % 20 with fixed divisors 1/2/20 assumes exactly 200 Hz tick (HUB-9).
 - 138-207: frames are sent every period whether or not new AFE data arrived; no staleness/valid flag, no sequence counter (the requirement mentions ID counter bits: none are implemented), so a stalled AFE (HUB-12) is invisible on CAN.
 - 192: Identifier is not masked to 29 bits (generated IDs are < 2^29 today).
 - Overlapping signal byte ranges inside a group are not detected (only bounds checked); signals for one group_id with different rates form separate groups with the same CAN ID (line 74): receivers cannot tell them apart.
 - Frame DLC mapping (39-69) and offset >= 4 timestamp rule (169) are correct; payload sized 64 and memset each time (ok).
 - HAL_GetTick() timestamp is ms in a 32-bit field, taken at queue time, not sample time.

**HUB-26 LOW** - Inc/afe_ctrl.h, Inc/tcan.h, Inc/sensor_state.h
 No include guards (works only because each is included once per TU). Generic global enum names (NOP, START, STOP, RREG, WREG, DEVICE_ID, REV_ID, INT_1..) pollute the namespace; tcan.h uses `size_t`? (tcan.c does, via main.h). The `HUB_TEMP_SAVE_WD_EEPROM` flag is defined in a header, so changing it rebuilds everything. Comments in tcan.h reference "TCAN2410Init" for a TCAN2411 part (cosmetic). sensor_state.h: AFEA_SENSORS/AFEB_SENSORS/DIG_SENSORS, `afe`, `afe_channel`, `sensor_type` are unused by firmware.

**HUB-27 LOW** - Src/hub_link.c, Src/hub_proto.c
 - hub_link.c:31 weak const HUB_ID is defined in the same TU as its use; the generated strong definition replaces it at link time, but the compiler may constant-fold 0xFF in `i->hub_id` (GCC treats weak definitions as non-foldable; fine today, fragile with LTO).
 - 69: HAL_UART_Transmit result ignored; if huart2.gState is busy the reply is silently dropped.
 - 142-145: on reboot the SBC is NOT reset; next boot goes through the "warm start" path (tcan.c:41) - works, but the reboot message means "MCU reset" only.
 - hub_proto.c:27 requires cap >= 64 but writes up to 2+56 (INFO) and 2+4+5*48 = 246 (LIVE_GET): safe only because on_msg passes 256; add cap checks.
 - 74: LIVE_GET accepts first >= 24 and returns count 0 (no error); protocol.md says slot=pin-1, 24 slots.
 - Stack on the INFO/ live path: Feed body[256] -> on_msg resp[256] -> send_msg out[260] -> Hframe_Encode body[256] ≈ 1.1 KB plus HubInfo; linker _Min_Stack_Size is only 0x400 (the stack is really all free RAM at the top, so it works, but the link-time check does not cover this path).

**HUB-28 LOW** - Src/main.c misc
 - 135-140 hexCharToVal returns 0 for invalid characters (silent).
 - 227-232 `reset` path: dead comment after HAL_NVIC_SystemReset; response is sent twice conceptually (230 then 236 unreachable).
 - 289-292 and 128-129: DMA half-transfer IT disabled after every restart (race window with the HAL enabling it); fine but duplicated.
 - 72-75, 100-244 everything protocol-related is inside `#ifdef DEF_DEBUG`, so a build without it has no config protocol at all and tcan_ok is dropped.
 - 304-317: TCAN2410Init() failure is only printed; the hub continues to run (SBC will cut 3V3 in ~600 ms anyway), no retry/blink/fallback.
 - 330-335 PA2 edge counter is polled at main-loop speed (diagnostic only).

## Dead / unreferenced / never-compiled code
- Src/debug_sniff.c lines 17-88 (see HUB-11): never compiled.
- Core/Inc/as.vhdl: does not exist.
- AFE acquisition: read_data_and_write_reg, process_spi_queue, HAL_GPIO_EXTI_Callback, HAL_SPI_TxRxCpltCallback are compiled but unreachable (no EXTI enable, no init) - HUB-1/2.
- CanScheduler_Init / Tick200Hz: compiled, never started (main.c:295-301 commented); HAL_TIM_PeriodElapsedCallback calls it only if TIM2 IRQ is enabled.
- TIM3 (init only), USART2 TX DMA channel (all TX is polling), SPI1/SPI2 DMA for SPI1 (TCAN is polled), enum entries WAKEUP/POWERDOWN/RDATA/SYOCAL/SYGCAL/SFOCAL/RREG and ADS124S08_REG_* in afe_ctrl.h, RREG, CRC_POLY_SET, INT_EN_* in tcan.h, tcan.h TCAN_INIT_* partially, sensor_state.h SampleRate/field members listed in HUB-26.
- Unused pins: DIN0-3, FAULT0-3, PG_5V, CAN_INT (TODO at tcan.c:12).
- `tcan_temp_save_result`, `HUB_TEMP_SAVE_WD_EEPROM` block (disabled).

## Checked and found fine
- hframe.c COBS encode/decode, CRC-16/CCITT-FALSE, buffer sizes (260 encoded / 256 body), bounds on every write; Hframe_Encode cap handling; hub_proto message layouts match protocol.md section 4 (INFO 56 B offsets, IDENT 18 B, LIVE_GET 5 B/slot, count <= 48 fits resp[256]).
- hub_link.c ring buffer index handling (single producer ISR / single consumer; 16-bit indices atomic on M4; overflow drops).
- UART swap: SWAP applied before DMA start, pads reconfigured consistently; HAL_UARTEx_ReceiveToIdle_DMA in DMA_NORMAL mode with restart per event is a valid pattern.
- ADS124S08 command opcodes and register addresses in afe_ctrl.h (NOP/WAKEUP/POWERDOWN/RESET/START/STOP/RDATA/SYOCAL/SYGCAL/SFOCAL, WREG=0x40|reg, RREG=0x20|reg, regs 0x00-0x11); WREG header (reg 0x02, count-1 = 6 writes INPMUX..VBIAS = the 7 SensorAfeConfig_t bytes in order); transfer length 9 fits the 9-byte buffers; packed struct layout equals register order.
- Generated DATARATE 0x1D = low-latency filter, 4000 SPS (single-conversion settling) - appropriate for round-robin muxing; 8-10 channels/AFE gives > 200 Hz per channel.
- can_scheduler.c: timestamp 4 bytes little-endian at [0..3], signal offset >= 4 enforced, length <= 4, end <= 64, FD DLC rounding (9-12 -> 12 ...), memset padding, rate dividers 1/2/20 vs 20-phase counter (correct for an exact 200 Hz tick), group count/array bounds, NULL/zero-length filtering.
- tcan.c: SPI frame format (reg<<1 | R/W, R/W=1 write), SPI mode 0, scratch-pad round trip before trusting reads, watchdog disable procedure (WD_CONFIG=00 in standby, WD_STBY_DIS bit, verify by readback), warm-start skip, SBC_CONFIG value 0x8A (normal, VCC2 on except sleep/failsafe, VCC1 OV default), save sequence per Table 8-25 (CRC enable, 0x8A with CRC 0x36, CRC disable with 0x5E, retry of CRC-off, EEPROM CRC check, force flag + 500-write warning), TCAN init occurs immediately after UART setup (only a few ms after reset, well inside the 600 ms tINITWD long window).
- Error_Handler disables IRQs and spins (hub then relies on the SBC; no IWDG is configured - consider one).
- All user-ISR code runs at priority 0 and none calls HAL_Delay/blocking APIs (debug_sniff is a stub); HAL_GetTick in ISRs is fine (read only).
