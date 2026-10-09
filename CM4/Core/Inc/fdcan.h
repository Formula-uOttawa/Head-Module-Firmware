#include "stm32h7xx_hal.h"
#include <stdint.h>

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs);