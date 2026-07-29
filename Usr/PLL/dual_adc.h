#ifndef DUAL_ADC_H
#define DUAL_ADC_H

#include "stm32f4xx_hal.h"
#include "pll_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DUAL_ADC_MIN_SAMPLE_RATE_HZ  PLL_MIN_SAMPLE_RATE_HZ
#define DUAL_ADC_MAX_SAMPLE_RATE_HZ  PLL_MAX_SAMPLE_RATE_HZ
#define DUAL_ADC_TIM2_CLOCK_HZ       PLL_TIM2_CLOCK_HZ

HAL_StatusTypeDef DualADC_Start(uint32_t *packed_pairs,
                                uint32_t pair_count,
                                uint32_t requested_rate_hz,
                                uint32_t *actual_rate_hz);
HAL_StatusTypeDef DualADC_Stop(void);
bool DualADC_IsRunning(void);

#ifdef __cplusplus
}
#endif

#endif /* DUAL_ADC_H */
