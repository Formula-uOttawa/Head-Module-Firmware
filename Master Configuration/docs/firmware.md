# Head module firmware architecture

```
 web app / dash                       CM7 (HeadModuleV1/CM7)                 CM4 (HeadModuleV1/CM4)
 ┌───────────────┐  UART4 (VCP)  ┌────────────────────────────┐        ┌───────────────────────────┐
 │ canharness    │ ───────────▶  │ head_link.c  (transport)   │        │ head_cm4.c                │
 │  device/      │  COBS+CRC     │  hframe.c   framing        │ SRAM4  │  FDCAN1/2 runtime config  │
 │  client.py    │               │  hproto.c   dispatcher     │◀──────▶│  RX ISR -> HeadDecode     │
 └───────────────┘  (UDP later:  │ head_store.c  flash sector │ shared │  head_decode.c  routing   │
                     same msgs)  │ head_debug.c  ASCII cmds   │ memory │  head_cfg.c    blob parse │
                                 └────────────────────────────┘        └───────────────────────────┘
```

## Where things live
| Path | Role | Cores |
|---|---|---|
| `Common/Src/head_cfg.c`, `Inc/head_cfg.h` | config blob parser/validator, CRC-32 | both |
| `Common/Src/head_decode.c` | frame routing: hub frames + custom CAN RX decode, staleness, stats | CM4 |
| `Common/Src/head_encode.c` | periodic transmit frames (Motec etc.) built from live values: bit insert, scaling, scheduler | CM4 |
| `Common/Src/hframe.c`, `hproto.c` | UART framing; transport independent message handler | CM7 |
| `Common/Src/hcan_timing.c` | FDCAN bit timing from a bit rate | CM4 |
| `Common/Inc/head_shared.h` | shared-memory layout (SRAM4 `0x38000000`), flash sector address | both |
| `Common/Src/head_default_blob.c` | compiled-in default config (generated: `python -m canharness generate system.json --deploy`) | both |
| `CM4/Core/Src/head_cm4.c` | runtime CAN setup, RX interrupt, mailbox, sniffer | CM4 |
| `CM7/Core/Src/head_link.c`, `head_store.c`, `head_debug.c` | UART link, flash storage, legacy ASCII commands | CM7 |

Hooks in generated files are limited to USER CODE sections (`main.c` of each core, `fdcan.c`).

## Configuration flow
1. The app builds a **config blob** (`canharness/blob.py`, format in `protocol.md`) from `system.json`.
2. Boot: the CM4 loads the newest valid slot of the flash log (`0x080E0000`, see protocol.md 3.2), otherwise the compiled-in default; if the FDCAN clock is not up yet it retries every 100 ms.
3. Runtime: the app uploads a blob (`CFG_BEGIN/CHUNK/COMMIT`). The CM7 CRC-checks and fully validates it, then either
   hands it to the CM4 (`HEAD_CMD_APPLY_STAGED`, apply) and/or writes it to flash (store).
4. The CM4 parses into the inactive `HeadConfig_t`, stops the buses, swaps the config pointer, re-initialises each FDCAN
   (bit timing from the kernel clock, filters from the blob, message RAM unchanged) and starts it. If anything fails, the
   previous config is restored and the error code returned.

Nothing about bus setup depends on CubeMX values other than pins, the FDCAN kernel clock and NVIC.

## Adding a transport (e.g. UDP via the dash)
Receive a datagram, call `Hproto_Handle(&proto, msg, len, resp, cap)` with `msg = type seq payload`, and send back the
returned bytes. No framing/CRC is needed (UDP checksum). Reuse `op_*` functions in `head_link.c` by moving the ops table
and `Hproto_t` instance to a shared file if both transports run at once; the staging buffer is single-instance, so uploads
over two transports at the same time are rejected by design (`CFG_BEGIN` restarts the upload).

## Memory and flash
* Flash bank 1 sector 7 (`0x080E0000`, 128 KB) holds the stored blob; the CM7 linker `FLASH` length is 896K to keep the
  image out of it. The CM7 stalls for ~1 s while erasing (it runs from the same bank); the CM4 runs from bank 2 and keeps
  receiving.
* SRAM4 (64 KB at `0x38000000`) is the mailbox/live-data area (~42 KB used). The CM4 initialises it and sets the magic
  last; the CM7 treats a missing magic as "CM4 not running". CM7 caches are off; enable an MPU non-cacheable region first
  if that ever changes.

## Building and testing
* Host tests (parser, decoder, framing, dispatcher, bit timing, API, UI backend): `cd "Master Configuration" && .venv/bin/python -m pytest tests`.
  They compile the real `Common/Src` files with the host C compiler.
* Firmware compile + link check without touching the repo (build output under `*/Debug/` is tracked in git):
  `tools/fw_build_check.sh CM7` and `tools/fw_build_check.sh CM4` (uses the toolchain bundled with CubeIDE).

## UART pin swap (board revision 1)
The ST-LINK VCP lines are wired TX-to-TX on board revision 1. `head_link.c` swaps UART4's TX/RX in hardware
(`HEAD_UART4_SWAP_TXRX`, default 1; set to 0 on a corrected board): the UART transmits on PC11 and receives on PC10, the
receiving pad is open-drain with a pull-up.

## Not implemented yet
* UDP transport and the UDP data stream to the dash (needs LwIP; requires an `.ioc` change).
* Hub config push over CAN (`HUB_CFG_*` message types are reserved; the hub firmware has no CAN receive path yet).
* TX of any frame from the head.

## Cross-core mailbox rules (see the header comment of `Common/Inc/head_shared.h`)
* The CM7 issues a request only when `cmd_req == cmd_ack` (otherwise `busy`, also after a timeout), fills `cmd`, `cmd_arg`, `cmd_len`, `cmd_crc` (for `APPLY_STAGED`: the blob header CRC, so a stale command is never run against a newer upload) and a non-zero `cmd_seq`, then a barrier, then increments `cmd_req`; it waits for the ack and checks `cmd_result_seq`.
* `HEAD_CMD_SET_SOURCE` tells the CM4 where the active config now lives (0 default, 1 flash, 2 RAM only). Only the CM4 writes `active_source`.
* Readiness = valid `magic` and a changed `cm4_heartbeat`; the CM7 never clears `magic`. `publish_seq` is a seqlock (odd = config swap in progress) that INFO and CFG_READ honour.
* The FDCAN RX interrupt only pushes frames (with the tick) into a 32-entry ring; decoding, statistics, sniffing and stale handling run in `HeadCm4_Poll`.
* HAL note: on the H7 HAL the FDCAN `DataLength` field is the plain DLC code 0..15 (not `code << 16`); a static assert guards this.
