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
  /* target_phase_deg is the effective target after calibration. */
  float target_phase_deg;
  float nominal_target_phase_deg;
  float phase_compensation_deg;
  float reference_frequency_hz;
  float dds_frequency_hz;
  float measured_phase_deg;
  float phase_error_deg;
  float phase_quality;
  float frequency_step_hz;
  float dds_phase_offset_deg;
  float phase_step_deg;
  float output_scale;
  bool fine_mode;
  bool direct_phase_mode;
  bool frequency_hold_mode;
  bool frequency_change_pending;
  uint8_t lock_band;
  uint16_t phase_pair_count;
  uint16_t block_pair_count;
  uint8_t analysis_stride;
  uint32_t sample_rate_hz;
  uint32_t dma_overrun_count;
  uint32_t adc_error_count;
  uint32_t uart_overflow_count;
  uint32_t dds_error_count;
  uint32_t frequency_reanchor_count;
  uint32_t search_restart_count;
  uint32_t lock_loss_count;
  uint32_t frequency_hold_enter_count;
  uint32_t frequency_hold_exit_count;
  uint32_t acquire_restart_suppressed_count;
} pll_demo_status_t;

HAL_StatusTypeDef PLL_Demo_Init(ad9910_t *dds);
HAL_StatusTypeDef PLL_Demo_Configure(uint8_t multiplier,
                                     float target_phase_deg,
                                     float output_scale);
HAL_StatusTypeDef PLL_Demo_Start(void);
HAL_StatusTypeDef PLL_Demo_Stop(void);
void PLL_Demo_Process(void);
const pll_demo_status_t *PLL_Demo_GetStatus(void);

/* ISR forwarding entry points. They only publish fixed-size events/bytes. */
void PLL_Demo_AdcHalfCpltISR(ADC_HandleTypeDef *hadc);
void PLL_Demo_AdcCpltISR(ADC_HandleTypeDef *hadc);
void PLL_Demo_AdcErrorISR(ADC_HandleTypeDef *hadc);

#ifdef __cplusplus
}
#endif

#endif /* PLL_DEMO_H */
