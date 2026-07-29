#ifndef PLL_DEMO_H
#define PLL_DEMO_H

#include "ad9910.h"
#include "stm32f4xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PLL_DEMO_STOPPED = 0,
  PLL_DEMO_SEARCHING,
  PLL_DEMO_ACQUIRING,
  PLL_DEMO_LOCKED,
  PLL_DEMO_ERROR
} pll_demo_state_t;

typedef struct {
  pll_demo_state_t state;
  uint8_t multiplier;
  float target_phase_deg;
  float reference_frequency_hz;
  float dds_frequency_hz;
  float measured_phase_deg;
  float phase_error_deg;
  float phase_quality;
  uint32_t sample_rate_hz;
  uint32_t dma_overrun_count;
  uint32_t adc_error_count;
  uint32_t uart_overflow_count;
  uint32_t dds_error_count;
} pll_demo_status_t;

HAL_StatusTypeDef PLL_Demo_Init(ad9910_t *dds);
void PLL_Demo_Process(void);
const pll_demo_status_t *PLL_Demo_GetStatus(void);

/* ISR forwarding entry points. They only publish fixed-size events/bytes. */
void PLL_Demo_AdcHalfCpltISR(ADC_HandleTypeDef *hadc);
void PLL_Demo_AdcCpltISR(ADC_HandleTypeDef *hadc);
void PLL_Demo_AdcErrorISR(ADC_HandleTypeDef *hadc);
void PLL_Demo_UartRxCpltISR(UART_HandleTypeDef *huart);
void PLL_Demo_UartTxCpltISR(UART_HandleTypeDef *huart);
void PLL_Demo_UartErrorISR(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* PLL_DEMO_H */
