/* head_cm4.h - CM4 side of the head firmware: runtime-configurable CAN receive + decode. */
#ifndef HEAD_CM4_H
#define HEAD_CM4_H

#include "main.h"

/* Initialise shared memory, load the stored (or default) config and start the CAN buses. Call once after MX_*_Init(). */
void HeadCm4_Init(void);
/* Housekeeping: mailbox commands from the CM7, staleness, bus status, sniff mode. Call from the main loop. */
void HeadCm4_Poll(void);
/* Call from HAL_FDCAN_RxFifo0Callback. */
void HeadCm4_OnRxFifo0(FDCAN_HandleTypeDef *hfdcan, uint32_t rxFifo0ITs);

#endif
