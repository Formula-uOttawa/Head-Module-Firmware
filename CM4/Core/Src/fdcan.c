#include "fdcan.h"
#include "head_cm4.h"

/* All received frames (hub bus and external buses) go through the runtime-configured decoder in head_cm4.c. */
void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    HeadCm4_OnRxFifo0(hfdcan, RxFifo0ITs);
}
