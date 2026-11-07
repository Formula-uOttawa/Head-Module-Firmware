# CAN-harness head module: status & todo

Last updated: 2026-11-05 (hub config over CAN)

Legend: ✅ done · 🔧 in progress / needs your input · 📋 planned · 💡 idea / maybe

## Open decisions / waiting on you
- 🔧 **Test CAN on hardware** once something is connected: bit timing (320 kbit/s nominal, 5 Mbit/s data at 80 MHz), RX filters, hub frame decode, custom receive frames, sniffer.
- 🔧 **Confirm the clocks on hardware.** You changed both FDCAN clocks to 80 MHz. The Device tab shows the head's real FDCAN clock and flags a mismatch with the project. For the hub, the shared PLL also changed SYSCLK, so check the hub's SYSCLK-derived timing (TIM2 200 Hz tick, AFE SPI prescaler, ADC clock).
- 🔧 **Config upload/store on hardware:** upload, apply and store a config from the Device tab, power cycle, check it reloads from flash.

- 🔧 **Test Motec transmit on hardware:** define a frame in the CAN transmit tab, upload, and check the sniffer/Motec receives it. Motec's byte order/scale settings: use the tab's Export CSV. Analog hub channels are float32 volts (the head converts the ADC code with the project's ADC reference voltage, 5 V by default); values are sent as `(value - offset) / multiplier`.

- ✅ **Config ownership decided (recommendation, say if you disagree):** the app/project is the single source of truth; head and hubs are both targets. Hub config goes to the hub directly over its UART (bench) or via the head over CAN; devices report their config hash so outdated ones are visible. No hub-to-head propagation. Details in `docs/protocol.md` section 4.
- 🔧 **Hub AFE B does not answer on this board** (boot line `afe:B:stage=12`, status `0x35`; chip looks off-centre on its pad, check solder/CS_B PC8/DOUT/supplies). The hub keeps running on AFE A: the failed init stays a severe error (boot line, INFO `AFE fault` flag, red status on the Device tab, retried every 5 s, `afe:status:` line if it recovers) and B's channels are sent on CAN as the "no data" code -8388608 (also for any AFE channel that goes stale). The head/dash must treat -8388608 as invalid (dash JSON: `no_data_value`).
- 🔧 **Hub PA2 (USART2 RX) pin looks dead**, so the hub can send but not receive over UART; its config now goes over CAN through the head (below). UART fix, if wanted: replace the MCU, or wire the ST-LINK TX to PB3 and receive there.
- ✅ **Hub AFE B works after the resolder** (`afe status 0x3F`).

**Hub console over CAN (2026-11-06):** with a hub selected on the head's Device tab, the console box runs commands on the hub through the CAN relay (`HUB_ASCII`), e.g. `ping`, `tcan:wd`, `afe:probe:a` (restarts that AFE and reads it without the DRDY interrupt: codes change + `drdy_low=0` means the DRDY wire/pin is at fault; the hub INFO also shows per-AFE samples / DRDY edges / resyncs). Needs a hub reflash only.

**Channel frame/offset removed (2026-11-06):** channels no longer have `frame_idx` / `byte_offset`; packing is always automatic (the packer's own frame index and offsets in the Frames view, dash JSON and blobs are unchanged). Old project files that still carry the two fields load fine and drop them on the next save.

**Live views (2026-11-06):** the hub's live table shows only pin, input, channel, raw code and status (no volts / calibrated / units). The head's live table has a Fault column: it compares the Calibrated-column value (raw volts for an analog channel without calibration) against `fault_low` / `fault_high` and shows `FAULT low/high`; with either limit missing it shows "OK (no fault limit)", stale channels show "—". App only, no reflash.

**Hub: over-range no longer looks like "no sample" (2026-11-06):** the hub clamps a negative over-range ADC code (0x800000, e.g. an open or over-driven input) to -8388607, because -8388608 is the bus's "no sample" marker and the head drops it (the channel then counts as not received and goes stale). AFE B throughput check: with the low-latency filter at 100 SPS each channel switch costs the 10.156 ms first-conversion time (datasheet Table 13), so N channels on AFE B give at most about 98 / N Hz per channel (9 channels: 10.9 Hz); the hub's rate check uses 10 ms + 30 us per channel. Needs the hub reflashed.

**CM4 tick ran 3.125x fast (fixed in code, 2026-11-06):** every channel read about 32 % of its assigned rate (200 Hz as 64 Hz). Cause: the dual-core boot sync define is off, so the CM4 runs `HAL_Init` while the clock is still the 64 MHz reset default (SysTick reload for 64 MHz) and the CM7 then raises HCLK to 200 MHz, making the CM4's "millisecond" 0.32 ms. That also made every CM4 millisecond timeout (channel stale timeouts, hub relay timeout, transmit periods to the Motec ECU, boot retry) 3.125x too short or fast. `sync_systick()` in `head_cm4.c` recomputes the core clock at init and every 100 ms and re-arms SysTick when it differs. Needs the CM4 reflashed; confirm the rates then read about the assigned values. The CM7 is not affected (its clock config re-arms its tick).

**Live reception rate (2026-11-06):** the head counts the frames that carried a fresh sample of each channel (`HeadLive_t.rx_count`) and the CM4 turns that into a rate every 2 s (`HeadDecode_UpdateRates`, 0.1 Hz units). LIVE_GET entries on the head are 7 bytes now (`u32 value, u8 flags, u16 rate_x10`, at most 32 per request; the app still reads old 5-byte replies). The Device live view has a "Rate (Hz)" column: measured / assigned rate, with a "low" chip below 90 % of the assigned rate. A channel the hub marks "no sample" does not count. Needs both head cores reflashed; the hub's own live view is unchanged.

**Resistance is calibration (2026-11-06):** the ohm special case (IDAC current, series resistor, route offset) was removed again; every analog hub channel is volts in the head and the live view, and resistive channels convert via the normal calibration curve. The live view's Value column shows the raw volts and the Calibrated column the calibrated value with the configured units ("—" for analog channels without a calibration). Needs a new head upload and the regenerated `head_default_blob.c` (done).

**Volts in the head (2026-11-06; fixed decimals and fixed column widths in the live view):** analog hub channels are converted to float32 volts at the sensor pin in the head (`code / 2^23 * Vref / gain`; Vref = project setting `afe_vref_v`, default 5 V = REFP0 tied to the 5 V rail; thermocouples use the internal 2.5 V and gain 128). The hub's "no sample" code -8388608 is dropped (channel goes stale). Live values in the app, the dash JSON (`value_type f32`, `volts_per_count`) and Motec transmit follow; calibration x values are in volts. Needs a new head upload (blob route elements are 16 bytes now) and both head cores reflashed. If the 5 V reference is not exactly 5 V, set the measured value in Hubs & settings. Jumper test of the inputs: only on active-type channels, through 1-10 kohm, and only if AVDD is about 5 V (absolute maximum AVDD + 0.3 V).

## Hub config over CAN (2026-11-05): implemented and host-tested, NOT yet run on hardware
The hub now has the same configuration architecture as the head: the app builds a **hub blob** (`canharness/hubblob.py`, `protocol.md` 2.1), the hub parses it into RAM tables at boot
from the newest valid slot of its own **flash log** (last 4 pages of bank 2, `0x0807E000`), or from a compiled-in default (`hub_default_blob.c`) if none. `sensor_config_generated.c`
is gone. The app reaches a hub **through the head**: UART -> head (`HUB_RELAY`) -> CAN-FD hub bus -> hub (`hcanlink`, `protocol.md` 3.4). The same hub protocol also works on the hub's UART (bench, TX only on this board).
161 host tests pass (real `hcanlink.c`, `head_relay.c`, `hproto.c` and the hub's `hub_canlink.c`, `hub_proto.c`, `hub_cfg.c`, `hub_slots.c`, `hub_store.c` run together over a simulated bus with a RAM flash model, including lost/duplicated fragments, power loss during a store, log wrap-around); all three firmware builds link without warnings.

**Do these (in order):**
1. CubeIDE: refresh both projects (new files: head `Common/{Inc,Src}/{hcanlink,head_relay}`; hub `Core/{Inc,Src}/{hcanlink,hub_canlink,hub_cfg,hub_flash,hub_slots,hub_store}`, `hub_default_blob.c`; `sensor_config_generated.c` was removed). Flash **both head cores and the hub**, power-cycle the head.
2. For every hub board: `python -m canharness generate system.json --deploy-hub <its id>` and flash that build, so a fresh board answers with its own id (two boards with the same compiled-in id would both answer a request). The deployed file is `SensorHubV1/Core/Src/hub_default_blob.c` (hub 0 is deployed now).
3. Web app (restart the server): connect to the **head**, Device tab, "Scan for hubs", Open the hub, then "Upload & restart hub". The hub screen then shows the active/stored config hashes ("matches project") and the CAN config link counters.
4. Hardware assumptions I could not check: the hub's FDCAN message RAM is re-initialised in user code with one extended filter element (`HubCanLink_Init`; the `.ioc` still says 0 extended filters, setting it to 1 in CubeMX would make this consistent); the hub's flash must be in dual bank mode (default option byte `DBANK=1`, otherwise `HubStore_Write` returns "flash error"); flash stalls, ECC behaviour and the head's relay/ACK filter are untested; the hub and head must agree on the 5 Mbit/s bit timing as for the sensor frames.
5. Linker: the log sits in the last 4 pages of bank 2 without a linker reservation (the image is ~72 KB, far below). A runtime check refuses writes if the image ever reaches that area. If you want it enforced at link time, reduce the hub's `FLASH` length by 8 KB in `STM32G474RETX_FLASH.ld` (needs your permission).

Known limits: a torn flash page can still raise a double-ECC fault when read at boot (same risk as the head, see below); a hub that is renamed by a blob answers only to its new id; one relay in flight at a time (1 s timeout); the hub applies a new config only after a reset (no live apply, by design); the app builds the hub blob from the project's hub id, so a hub flashed with a wrong default id has to be reflashed or renamed by uploading a blob for the new id to the old id.

**Debugging a CAN bus-off (head):** the ASCII command `can:status` prints one line per FDCAN: TEC/REC, bus-off/passive, the error types seen since the config was applied (`lec` arbitration phase, `dlec` data phase: stuff, form, ack, bit1, bit0, crc), the raw bit timing registers and the measured delay (`tdcv`). Reading: only `ack` = nobody acknowledges (wiring/other node); `bit0`/`bit1` = the controller does not read back what it drives (transceiver off, RX line, or the data-phase delay compensation); `form`/`crc`/`stuff` = bit rate mismatch or noise. Needs both head cores reflashed.

## Code review fixes (2026-11-05): implemented, NOT yet run on hardware
Everything from the review (`reviews/*.md`; `STATUS` list before this change) was fixed except **HC-1 CAN bus-off recovery** (declined: new feature). 128 host tests pass; all three firmware builds link with no warnings in project/user files. None of it has been exercised on the boards: **please flash head and hub and go through the checklist below.**

**Important discovery while fixing (not in the reviews):** the old head CM4 code assumed the H7 HAL's FDCAN `DataLength` is `code << 16`; in this HAL it is the plain DLC code 0..15. Every received frame had length 0 (hub frames with `min_len > 0` never matched) and TX read past a table. Fixed, with a static assert so a HAL change breaks the build. "No hub data arrives" on the head may well have been this.

**Do these (in order):**
1. In CubeIDE: refresh/clean both projects (new files: head `CM7/Core/Inc/{head_config,head_text}.h`, `Common/{Inc,Src}/head_{slots,rxq}`, `head_util.h`; hub `Core/Inc/{hub_config,live_slots}.h`, `Core/Src/live_slots.c`). The out-of-tree scripts glob them already.
2. Hub: deploy the hub default with `generate system.json --deploy-hub <id>` (see the hub config section above; it replaces the old `sensor_config_generated.c` step). Head: `Common/Src/head_default_blob.c` was regenerated; restart the web server (`python -m canharness.web`) for the new app code.
3. Flash both boards (bench supply first time if the SBC needs it). Check: head boot line `boot:tcan:OK:tries=1:init=..`; hub `tcan:init:OK`; Device tab shows the hub's new Acquisition / System basis chip rows.
4. Hub hardware assumptions I could not verify (check on the bench): both ADS124S08 START pins held low (hub uses the START command, no START pin symbol in `main.h`), DRDY idles high, the SBC supplies the AFEs before `AfeInit` runs, DIN/FAULT polarities. TX delay compensation offset for 5 Mbit/s (150 ns) is a calculation, not measured. `.ioc` has no SPI2 interrupt, so SPI overrun errors rely on the 5 ms stuck-transfer watchdog.
5. Head: check the new flash store (upload + store several times, power cycle, `flash` ASCII command shows the slots), and a CAN round trip (RX decode now uses real lengths; TX frames to the Motec).

**What changed (summary):**
- Hub: AFE bring-up (`AfeInit`: reset release, ID check, START), correct A/B addressing, SPI2 mode 1 (`.ioc` + generated, permitted), EXTI9_5 for DRDY (`.ioc` + generated, permitted), TIM2 exactly 200 Hz and TIM3 1 us (`.ioc` + generated, permitted), CAN_CNTRL_1 written (transceiver on), start order SBC -> AFE -> FDCAN -> tick behind `HUB_ENABLE_ACQUISITION` (default 1, `hub_config.h`), TX delay compensation, FIFO-aware scheduler with staggered groups, per-hub frame counter in the CAN ID, 24-bit signed raw values, digital inputs sampled, SPI/DMA error handling + no-sample watchdog + per-slot staleness, finite SPI timeouts, debug console hardened (NULL checks, whitelist, `sniff:` really works now), heartbeat off by default (`diag:on`), runtime `tcan:save` removed (compile-time one-shot stays), include guards and prefixed enums.
- Head Common/CM4: RX interrupt only queues frames (decode in the main loop), runtime sniff toggle works, FDCAN2 8-byte element limits enforced, config validation (dlc set, peripherals, filters, bit rates, route extent, filter coverage, NaN/Inf), apply validated before the buses are stopped and rollback checked, seqlock for the published config, boot-time apply retry while the FDCAN clock is not ready, single-precision TX encode, exact integer path for 32-bit channels, staggered TX with retries, stale logic fixed, `hub_lost` duplicate handling, table CRC, frame parser resync fix (shared by head and hub).
- Head CM7: flash config is now an append-only log of 7 slots (power-loss safe, identical stores skipped, `docs/protocol.md` 3.2), mailbox hardened (busy while outstanding, sequence ids, CM4 owns `active_source`), SBC init ported from the hub (retries, verified watchdog disable, CAN_CNTRL_1, `HEAD_TEMP_SAVE_WD_EEPROM` one-shot flag default 0), early UART pin swap, link counters in INFO, text-line filter, `send:tcan` whitelist, `tcan:wd` and `flash` read-only commands.
- App/protocol: REBOOT needs the magic `0xB007F00D`; COMMIT reply has a trailing `store_err` byte (the Device tab warns when a config is active but not stored); INFO/STATS/LIVE additions parsed and shown; validator: analog channels must be signed 3-4 bytes, FD only on FDCAN1, bit-rate limits; the packer no longer wastes bytes on 3-byte fields; legacy import upgrades analog channels. `system.json` analog channels are now 3 signed bytes.

**Known gaps / not done:**
- HC-1 bus-off recovery (declined). CAN bus-off is only reported.
- A torn flash word read can still raise a double-ECC NMI (`NMI_Handler` is `while(1)` in generated code); mitigated by the commit-word design but untested. Needs a hardware test (and your permission to touch the NMI handler/FLASH interrupt setup).
- Hub: a failed SBC init is only reported (no retry/blink, CAN start is skipped); per-sensor `sample_rate` is not used to schedule the AFE round robin (rates are validated at init; AFE B has only ~10% margin); digital slots hold the pin level, not a frequency; no IWDG; no FDCAN bus-off recovery.
- The reviewer claim HUB-5 (AFE data read during WREG) was wrong: the datasheet describes exactly that pattern (read-data-direct), left as is and documented.
- Head: system reset (REBOOT / `reset`) still cuts the CM4 mid-apply or mid-transmit; the head's UART pin swap is still software (`.ioc` untouched on the head).
- Host tests cannot run `head_cm4.c`, `head_link.c`, `head_store.c`, `tcan.c`, `afe_ctrl.c` HAL code (compile-checked only).

## Done
### Config app (`Master Configuration/`)
- ✅ Project model, validation (pins, AFE throughput, frame packing, bus rates vs FDCAN clocks, CAN RX frames, filter capacity)
- ✅ CAN ID standard from `FSAE Sensor Hub CAN Tables.xlsx`; packer using the full 64 B frame
- ✅ Generators: hub C (`sensor_config_generated.c`), head config blob + compiled-in default (`generate --deploy`), dash `channels.json`
- ✅ Web UI: channels, CAN receive (custom frames, bit map), connectors, frames and bus load, calibration, hubs and settings, device
- ✅ Device tab: connect, upload (apply or store), read back, revert, reboot, live values, bus stats, CAN sniffer with "define frame"
- ✅ Device page feedback: top bar turns green while the device answers (amber/red when it stops), status and bus stats refresh every 2 s, console log with repeat counts and ASCII command input, temporary error toasts
- ✅ Device link diagnostics: HTTP/server errors carry details, timeouts say whether bytes arrived (or failed CRC), macOS `tty.*` duplicate ports hidden
- ✅ 60 tests (they compile and run the real firmware modules from `Common/` and the hub)

### Hub firmware (`../SensorHubV1`)
- ✅ Cleanup pass: removed the legacy DMA debug prototype (head CM7 `main.c`), redundant FDCAN notification calls (head CM4 `main.c`), dead code in hub `tcan.c`/`main.c` (unused read, empty `if`, `new_cmd_rdy`, commented heartbeat). WIP modules (`afe_ctrl.c`, `can_scheduler.c`) left as they are.
- ✅ UART link on USART2 with the TX/RX pin swap (like the head), binary protocol + ASCII commands, non-blocking main loop (the 500 ms `HAL_Delay` is gone)
- ✅ Protocol: IDENT, INFO (clocks, CAN error counters, scheduler counters, TCAN register), LIVE_GET (24 pin slots), REBOOT
- ✅ App detects head vs hub from IDENT and shows the matching Device screen (hub: info, project cross-checks, live pin values)
- ✅ `tools/hub_build_check.sh` compile/link check of the hub project
- ✅ Runtime hub config: blob + flash log + CAN receive (`hub_canlink.c`) + CFG messages on UART and CAN; head relay (`HUB_RELAY`, CM4 `relay_begin`); app: hub list/scan/open in the Device tab, hub blob upload, `--deploy-hub`

### Head firmware (`Common/`, `CM4/`, `CM7/`)
- ✅ Motec/ECU transmit: periodic TX frames (11/29-bit, 1-1000 Hz, up to 8 B on classic buses) with signal packing (Intel/Motorola, signed, float, multiplier/offset), shared FDCAN2 with RX, TX counters in stats; app has a CAN transmit tab (auto-arrange, CSV export)
- ✅ Config blob parser, CAN decode and routing (hub frames and custom frames, mux, Motorola/Intel, float), staleness, stats
- ✅ CM4: runtime FDCAN setup from the config (bit timing, filters), RX interrupt, mailbox, sniffer
- ✅ CM7: UART4 link, COBS+CRC framing, protocol dispatcher, flash config storage (bank 1 sector 7), legacy ASCII commands
- ✅ UART link tested on hardware (software TX/RX swap works: timestamps and commands answered)
- ✅ Compile and link check of both cores (`tools/fw_build_check.sh`)
- ✅ Docs: `Master Configuration/docs/protocol.md`, `docs/firmware.md`

## Planned / todo
### Head
- 📋 UDP transport and data stream to the dash (needs LwIP via .ioc: **ask permission first**) and a UDP payload format decision
- 📋 TX options not built: send-on-change, per-signal stale handling (today a stale channel is sent as its last value / 0), calibrated-value sending
- 📋 MPU non-cacheable region for SRAM4 if the CM7 data cache is ever enabled
- 💡 DBC import for custom CAN receive frames (needs `cantools`)

### Hub (`../SensorHubV1`)
- 📋 Hub stats/sniffer messages if wanted; hub live-value staleness
- 📋 "other" ID bits [21-28] at TX and `RATE_50HZ`/`RATE_1000HZ` support on the hub

### App
- 📋 Deploy buttons in the UI (hub and head default blobs; today `generate --deploy` / `--deploy-hub <id>` on the command line)
- 📋 Hub-side bit timing in the project (the hub's CAN bit timing is still compiled in from CubeMX)
- 📋 Hub status-stream mode, mark an AFE as not fitted, head/dash handling of the -8388608 no-data code
- 💡 Show the on-device config diff against the project

## Notes
- Head repo: all work so far is committed (latest: review fixes). The hub (`../SensorHubV1`) is its own git repo now (single "Re init" commit plus whatever you commit next).
- The web server does not hot-reload: restart `python -m canharness.web` after pulling/changing app code (a stale server makes the Device page report "HTTP 404 ... server is older than this page").
- **The head has two images: flash BOTH `CM7` and `CM4`, then power-cycle.** Flashing only the CM7 leaves an old CM4 image, and the shared-memory layout changed: symptoms are `CFG_BEGIN busy`, `SNIFF_SET out of range`, `CFG_READ unsupported` (the CM7 sees no ready CM4).
- Build output under `*/Debug/` is tracked in git, so build in CubeIDE or use `tools/fw_build_check.sh`.
- `CM4/Core/Src/main.c` and `CM7/Core/Src/main.c` use CRLF line endings.
- Permission needed before touching: `.ioc`, generated code, linker scripts, `.cproject` (granted once: CM7 FLASH 896K, `../../Common/Inc` include path).
