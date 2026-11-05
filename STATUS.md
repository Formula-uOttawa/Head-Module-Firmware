# CAN-harness head module: status & todo

Last updated: 2026-11-08

Legend: ✅ done · 🔧 in progress / needs your input · 📋 planned · 💡 idea / maybe

## Open decisions / waiting on you
- 🔧 **Test CAN on hardware** once something is connected: bit timing (320 kbit/s nominal, 5 Mbit/s data at 80 MHz), RX filters, hub frame decode, custom receive frames, sniffer.
- 🔧 **Confirm the clocks on hardware.** You changed both FDCAN clocks to 80 MHz. The Device tab shows the head's real FDCAN clock and flags a mismatch with the project. For the hub, the shared PLL also changed SYSCLK, so check the hub's SYSCLK-derived timing (TIM2 200 Hz tick, AFE SPI prescaler, ADC clock).
- 🔧 **Config upload/store on hardware:** upload, apply and store a config from the Device tab, power cycle, check it reloads from flash.

- 🔧 **Test Motec transmit on hardware:** define a frame in the CAN transmit tab, upload, and check the sniffer/Motec receives it. Motec's byte order/scale settings: use the tab's Export CSV. Values are sent raw (hub channels = ADC counts) with your multiplier/offset.

- 🔧 **Hub bring-up on hardware:** flash the hub, connect in the Device tab (it should show the hub screen), check the UART pin swap works (`HUB_UART2_SWAP_TXRX` in `hub_link.c`), CAN kernel clock, and that `Hello World!` text and ASCII commands (`ping`) still work. Regenerate hub C (`generate`) and copy it to the hub so `HUB_ID` is defined (otherwise the hub shows "hub ID unknown").
- ✅ **Config ownership decided (recommendation, say if you disagree):** the app/project is the single source of truth; head and hubs are both targets. Hub config goes to the hub directly over its UART (bench) or via the head over CAN; devices report their config hash so outdated ones are visible. No hub-to-head propagation. Details in `docs/protocol.md` section 4.

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
- 📋 Frame counter and "other" ID bits at TX (ID standard), and `RATE_50HZ`/`RATE_1000HZ` support
- 📋 Bugs found earlier: `afe_ctrl.c` passes `afe_is_A = 0` in both branches (CS_A is never used), no AFE init/start, digital inputs never read, FAULT pins unused, CAN start commented out in `main.c`
- 📋 Deploy the generated hub C into `SensorHubV1/Core/Src` (app has no hub deploy step yet)

### App
- 📋 Deploy buttons in the UI (hub C, head default blob)
- 📋 Hub-side bit timing in the project once the hub has runtime config
- 💡 Show the on-device config diff against the project

## Notes
- Nothing is committed from this work; see `git status`.
- The web server does not hot-reload: restart `python -m canharness.web` after pulling/changing app code (a stale server makes the Device page report "HTTP 404 ... server is older than this page").
- Build output under `*/Debug/` is tracked in git, so build in CubeIDE or use `tools/fw_build_check.sh`.
- `CM4/Core/Src/main.c` and `CM7/Core/Src/main.c` use CRLF line endings.
- Permission needed before touching: `.ioc`, generated code, linker scripts, `.cproject` (granted once: CM7 FLASH 896K, `../../Common/Inc` include path).
