/* head_link.h - CM7 side of the head firmware: UART link, config/debug protocol, flash config storage. */
#ifndef HEAD_LINK_H
#define HEAD_LINK_H

/* Swap UART4 TX/RX and the pads to match the board wiring. Call right after MX_UART4_Init() (before anything slow), so the pads
   stop contending with the ST-LINK as early as possible. Idempotent; HeadLink_Init() calls it too. */
void HeadLink_PinSwap(void);
/* Start the UART4 receiver and set up the protocol. Call once after MX_UART4_Init() and HeadLink_PinSwap(). */
void HeadLink_Init(void);
/* Process received bytes and stream sniffed frames. Call from the main loop. */
void HeadLink_Poll(void);
/* Send raw text on the link (used by the ASCII debug commands). */
void HeadLink_SendText(const char *text);

#endif /* HEAD_LINK_H */
