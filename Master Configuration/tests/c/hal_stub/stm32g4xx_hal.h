/* Host stand-in for the parts of the STM32 HAL that the hub firmware's portable-ish modules use (can_scheduler.c,
 * afe_ctrl.c, live_slots.c). The simulators (tests/c/sched_sim.c, afe_sim.c) implement the stubs declared here. */
#ifndef STUB_STM32G4XX_HAL_H
#define STUB_STM32G4XX_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
typedef struct { int id; } GPIO_TypeDef;
extern GPIO_TypeDef stub_gpioa, stub_gpioc, stub_gpiod;
#define GPIOA (&stub_gpioa)
#define GPIOC (&stub_gpioc)
#define GPIOD (&stub_gpiod)
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin);
#define __HAL_GPIO_EXTI_CLEAR_IT(pin) ((void)(pin))

uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t ms);

/* interrupt masking: the simulators are single threaded */
static inline uint32_t __get_PRIMASK(void) { return 0; }
static inline void __set_PRIMASK(uint32_t v) { (void)v; }
static inline void __disable_irq(void) {}

/* SPI */
typedef struct { int id; } SPI_TypeDef;
extern SPI_TypeDef stub_spi2;
#define SPI2 (&stub_spi2)
#define SPI_POLARITY_LOW 0u
#define SPI_PHASE_1EDGE 0u
#define SPI_PHASE_2EDGE 1u
typedef struct { uint32_t CLKPolarity, CLKPhase; } SPI_InitTypeDef;
typedef struct { SPI_TypeDef *Instance; SPI_InitTypeDef Init; } SPI_HandleTypeDef;
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *h);
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *h);
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *h);

/* FDCAN (DLC codes equal the HAL's order: 0..8 = bytes, 9 = 12, 10 = 16, 11 = 20, 12 = 24, 13 = 32, 14 = 48, 15 = 64) */
#define FDCAN_DLC_BYTES_0 0u
#define FDCAN_DLC_BYTES_1 1u
#define FDCAN_DLC_BYTES_2 2u
#define FDCAN_DLC_BYTES_3 3u
#define FDCAN_DLC_BYTES_4 4u
#define FDCAN_DLC_BYTES_5 5u
#define FDCAN_DLC_BYTES_6 6u
#define FDCAN_DLC_BYTES_7 7u
#define FDCAN_DLC_BYTES_8 8u
#define FDCAN_DLC_BYTES_12 9u
#define FDCAN_DLC_BYTES_16 10u
#define FDCAN_DLC_BYTES_20 11u
#define FDCAN_DLC_BYTES_24 12u
#define FDCAN_DLC_BYTES_32 13u
#define FDCAN_DLC_BYTES_48 14u
#define FDCAN_DLC_BYTES_64 15u
#define FDCAN_EXTENDED_ID 1u
#define FDCAN_DATA_FRAME 0u
#define FDCAN_ESI_ACTIVE 0u
#define FDCAN_BRS_ON 1u
#define FDCAN_FD_CAN 1u
#define FDCAN_NO_TX_EVENTS 0u
typedef struct {
    uint32_t Identifier, IdType, TxFrameType, DataLength, ErrorStateIndicator, BitRateSwitch, FDFormat, TxEventFifoControl, MessageMarker;
} FDCAN_TxHeaderTypeDef;
typedef struct { int id; } FDCAN_HandleTypeDef;
uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h);
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h, FDCAN_TxHeaderTypeDef *hdr, uint8_t *data);

#endif
