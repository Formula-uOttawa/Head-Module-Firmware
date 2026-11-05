# CAN-harness head module: status & todo

Last updated: 2026-11-05 (review fixes)

Legend: ✅ done · 🔧 in progress / needs your input · 📋 planned · 💡 idea / maybe

## Open decisions / waiting on you
- 🔧 **Test CAN on hardware** once something is connected: bit timing (320 kbit/s nominal, 5 Mbit/s data at 80 MHz), RX filters, hub frame decode, custom receive frames, sniffer.
- 🔧 **Confirm the clocks on hardware.** You changed both FDCAN clocks to 80 MHz. The Device tab shows the head's real FDCAN clock and flags a mismatch with the project. For the hub, the shared PLL also changed SYSCLK, so check the hub's SYSCLK-derived timing (TIM2 200 Hz tick, AFE SPI prescaler, ADC clock).
- 🔧 **Config upload/store on hardware:** upload, apply and store a config from the Device tab, power cycle, check it reloads from flash.

- 🔧 **Test Motec transmit on hardware:** define a frame in the CAN transmit tab, upload, and check the sniffer/Motec receives it. Motec's byte order/scale settings: use the tab's Export CSV. Values are sent raw (hub channels = ADC counts) with your multiplier/offset.

- ✅ **Config ownership decided (recommendation, say if you disagree):** the app/project is the single source of truth; head and hubs are both targets. Hub config goes to the hub directly over its UART (bench) or via the head over CAN; devices report their config hash so outdated ones are visible. No hub-to-head propagation. Details in `docs/protocol.md` section 4.
- 🔧 **Hub AFE B does not answer on this board** (boot line `afe:B:stage=12`, status `0x35`; chip looks off-centre on its pad, check solder/CS_B PC8/DOUT/supplies). The hub keeps running on AFE A: the failed init stays a severe error (boot line, INFO `AFE fault` flag, red status on the Device tab, retried every 5 s, `afe:status:` line if it recovers) and B's channels are sent on CAN as the "no data" code -8388608 (also for any AFE channel that goes stale). The head/dash must treat -8388608 as invalid (dash JSON: `no_data_value`).
- 🔧 **Hub PA2 (USART2 RX) pin looks dead**, so the hub can send but not receive over UART; hub config goes over CAN for now (UART fix: replace the MCU, or wire the ST-LINK TX to PB3 and receive there).

## Code review fixes (2026-11-05): implemented, NOT yet run on hardware
Everything from the review (`reviews/*.md`; `STATUS` list before this change) was fixed except **HC-1 CAN bus-off recovery** (declined: new feature). 128 host tests pass; all three firmware builds link with no warnings in project/user files. None of it has been exercised on the boards: **please flash head and hub and go through the checklist below.**

**Important discovery while fixing (not in the reviews):** the old head CM4 code assumed the H7 HAL's FDCAN `DataLength` is `code << 16`; in this HAL it is the plain DLC code 0..15. Every received frame had length 0 (hub frames with `min_len > 0` never matched) and TX read past a table. Fixed, with a static assert so a HAL change breaks the build. "No hub data arrives" on the head may well have been this.

**Do these (in order):**
1. In CubeIDE: refresh/clean both projects (new files: head `CM7/Core/Inc/{head_config,head_text}.h`, `Common/{Inc,Src}/head_{slots,rxq}`, `head_util.h`; hub `Core/Inc/{hub_config,live_slots}.h`, `Core/Src/live_slots.c`). The out-of-tree scripts glob them already.
2. Hub: copy `Master Configuration/out/hub_<id>/sensor_config_generated.c` into `SensorHubV1/Core/Src/` (AFE channels are now 3 signed bytes; the old file still has 2-byte unsigned fields, so CAN would carry only bits 15:0 and the head would decode it differently). Head: `Common/Src/head_default_blob.c` was regenerated; restart the web server (`python -m canharness.web`) for the new app code.
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
- 📋 Hub config push needs a head TX path on the hub bus (the new TX scheduler is for periodic frames; FD/hub bus not enabled for it yet)
- 📋 TX options not built: send-on-change, per-signal stale handling (today a stale channel is sent as its last value / 0), calibrated-value sending
- 📋 MPU non-cacheable region for SRAM4 if the CM7 data cache is ever enabled
- 💡 DBC import for custom CAN receive frames (needs `cantools`)

### Hub (`../SensorHubV1`)
- 📋 Hub binary config format + runtime loading + flash storage; hub CFG_* messages (decided: app pushes to hub directly or via head over CAN)
- 📋 Hub stats/sniffer messages if wanted; hub live-value staleness
- 📋 Deploy step that copies generated hub C and `hframe.c/.h` into the hub project
- 📋 CAN receive path on the hub (filters, RX handler)
- 📋 Hub config push over CAN from the head (`HUB_CFG_*` messages reserved in the protocol), plus a binary hub config format and runtime loading
- 📋 "other" ID bits [21-28] at TX and `RATE_50HZ`/`RATE_1000HZ` support on the hub
- 📋 Deploy the generated hub C into `SensorHubV1/Core/Src` (app has no hub deploy step yet)

### App
- 📋 Deploy buttons in the UI (hub C, head default blob)
- 📋 Hub-side bit timing in the project once the hub has runtime config
- 💡 Show the on-device config diff against the project

## Notes
- Head repo: all work so far is committed (latest: review fixes). The hub (`../SensorHubV1`) is its own git repo now (single "Re init" commit plus whatever you commit next).
- The web server does not hot-reload: restart `python -m canharness.web` after pulling/changing app code (a stale server makes the Device page report "HTTP 404 ... server is older than this page").
- **The head has two images: flash BOTH `CM7` and `CM4`, then power-cycle.** Flashing only the CM7 leaves an old CM4 image, and the shared-memory layout changed: symptoms are `CFG_BEGIN busy`, `SNIFF_SET out of range`, `CFG_READ unsupported` (the CM7 sees no ready CM4).
- Build output under `*/Debug/` is tracked in git, so build in CubeIDE or use `tools/fw_build_check.sh`.
- `CM4/Core/Src/main.c` and `CM7/Core/Src/main.c` use CRLF line endings.
- Permission needed before touching: `.ioc`, generated code, linker scripts, `.cproject` (granted once: CM7 FLASH 896K, `../../Common/Inc` include path).
