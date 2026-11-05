/* head_link.h - CM7 side of the head firmware: UART link, config/debug protocol, flash config storage. */
#ifndef HEAD_LINK_H
#define HEAD_LINK_H

/* Start the UART4 receiver and set up the protocol. Call once after MX_UART4_Init(). */
void HeadLink_Init(void);
/* Process received bytes and stream sniffed frames. Call from the main loop. */
void HeadLink_Poll(void);
/* Send raw text on the link (used by the ASCII debug commands). */
void HeadLink_SendText(const char *text);

#endif
