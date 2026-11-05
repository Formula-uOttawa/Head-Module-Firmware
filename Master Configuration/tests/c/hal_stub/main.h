/* Host stand-in for the CubeMX main.h: pin names of the hub board (same values as the real one). */
#ifndef STUB_MAIN_H
#define STUB_MAIN_H
#include "stm32g4xx_hal.h"

/* sniff flags are defined by the application (main.c) */
extern volatile bool sniff_tcan, sniff_afeA, sniff_afeB;

#define CS_A_Pin ((uint16_t)(1u << 6))
#define CS_A_GPIO_Port GPIOC
#define DR_A_Pin ((uint16_t)(1u << 7))
#define DR_A_GPIO_Port GPIOC
#define CS_B_Pin ((uint16_t)(1u << 8))
#define CS_B_GPIO_Port GPIOC
#define DR_B_Pin ((uint16_t)(1u << 9))
#define DR_B_GPIO_Port GPIOC
#define RST_AB_Pin ((uint16_t)(1u << 8))
#define RST_AB_GPIO_Port GPIOA
#define DIN3_Pin ((uint16_t)(1u << 10))
#define DIN3_GPIO_Port GPIOC
#define DIN2_Pin ((uint16_t)(1u << 11))
#define DIN2_GPIO_Port GPIOC
#define DIN1_Pin ((uint16_t)(1u << 12))
#define DIN1_GPIO_Port GPIOC
#define DIN0_Pin ((uint16_t)(1u << 2))
#define DIN0_GPIO_Port GPIOD

#endif
