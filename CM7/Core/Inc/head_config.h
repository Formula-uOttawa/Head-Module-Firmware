/* head_config.h - build-time choices and constants of the CM7 user code (not CubeMX generated). */
#ifndef HEAD_CONFIG_H
#define HEAD_CONFIG_H

#define HEAD_FW_VERSION 0x0100u /* major << 8 | minor, reported in INFO / IDENT */

/*
 * HEAD_TEMP_SAVE_WD_EEPROM ONE-SHOT: 1 = at boot, after the SBC watchdog has been switched off (SBC still in standby), store that
 * setting in the SBC's EEPROM (TCANSaveWatchdogConfig). The result is printed as "boot:tcan:save=..." on the debug UART. Needed
 * once per board (the EEPROM takes only ~500 writes). Afterwards set it back to 0 and rebuild; leaving it at 1 is harmless (a
 * later boot reports "already saved"). There is deliberately no runtime command for it.
 */
#ifndef HEAD_TEMP_SAVE_WD_EEPROM
#define HEAD_TEMP_SAVE_WD_EEPROM 0
#endif

#define HEAD_TCAN_INIT_TRIES 3u      /* main.c: attempts of TCAN2410Init() before the boot continues without the SBC configured */
#define HEAD_TCAN_RETRY_DELAY_MS 10u /* pause between those attempts */

#define HEAD_UART4_NVIC_PRIO 5u      /* UART4 receive interrupt priority (group 0 sub 0 of the UART) */
#define HEAD_UART_TX_TIMEOUT_BASE_MS 5u /* UART transmit timeout = base + bytes / HEAD_UART_TX_BYTES_PER_MS (115200 baud ~ 11 B/ms) */
#define HEAD_UART_TX_BYTES_PER_MS 8u
#define HEAD_RX_IDLE_RESET_MS 50u    /* RX silence inside a binary frame after which the parser is reset */
#define HEAD_CM4_ALIVE_MS 500u       /* INFO: CM4 heartbeat must have changed this recently */
#define HEAD_CM4_CMD_TIMEOUT_MS 1500u /* wait for the CM4 to acknowledge one mailbox request */
#define HEAD_SEQ_RETRIES 4u          /* attempts of a consistent (seqlock) read of the CM4's active config */

#endif /* HEAD_CONFIG_H */
