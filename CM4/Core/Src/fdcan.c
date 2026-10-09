#include "fdcan.h"

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) != 0)
    {
        FDCAN_RxHeaderTypeDef rxHeader;
        uint8_t rxData[64];

        if (hfdcan->Instance == FDCAN1)
        {
            /* Retrieve 5 Mbps CAN-FD frame */
            HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rxHeader, rxData);
            
            // rxHeader.Identifier, rxHeader.DataLength, etc.
        }
        else if (hfdcan->Instance == FDCAN2)
        {
            /* Retrieve 500 kbps Classic CAN frame */
            HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rxHeader, rxData);
            
            // Process Classic CAN data
        }
    }
}