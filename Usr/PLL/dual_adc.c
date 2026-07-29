#include "dual_adc.h"

#include "adc.h"
#include "tim.h"

#include <limits.h>
#include <stddef.h>

static bool s_running;

static bool DualADC_ConfigureTimer(uint32_t requested_rate_hz,
                                   uint32_t *actual_rate_hz)
{
  uint64_t counts;

  if ((requested_rate_hz < DUAL_ADC_MIN_SAMPLE_RATE_HZ) ||
      (requested_rate_hz > DUAL_ADC_MAX_SAMPLE_RATE_HZ) ||
      (actual_rate_hz == NULL)) {
    return false;
  }

  counts = ((uint64_t)DUAL_ADC_TIM2_CLOCK_HZ +
            ((uint64_t)requested_rate_hz / 2ULL)) /
           (uint64_t)requested_rate_hz;
  if ((counts == 0ULL) || (counts > ((uint64_t)UINT32_MAX + 1ULL))) {
    return false;
  }

  __HAL_TIM_DISABLE(&htim2);
  __HAL_TIM_SET_PRESCALER(&htim2, 0U);
  __HAL_TIM_SET_AUTORELOAD(&htim2, (uint32_t)(counts - 1ULL));
  htim2.Instance->EGR = TIM_EGR_UG;
  __HAL_TIM_SET_COUNTER(&htim2, 0U);
  __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);

  *actual_rate_hz =
      (uint32_t)(((uint64_t)DUAL_ADC_TIM2_CLOCK_HZ + (counts / 2ULL)) /
                 counts);
  return true;
}

HAL_StatusTypeDef DualADC_Stop(void)
{
  HAL_StatusTypeDef result = HAL_OK;
  HAL_StatusTypeDef status;

  if (!s_running) {
    return HAL_OK;
  }

  /* Remove the trigger first so neither ADC can advance during cleanup. */
  __HAL_TIM_DISABLE(&htim2);
  status = HAL_TIM_Base_Stop(&htim2);
  if (status != HAL_OK) {
    result = status;
  }

  status = HAL_ADCEx_MultiModeStop_DMA(&hadc1);
  if ((status != HAL_OK) && (result == HAL_OK)) {
    result = status;
  }

  status = HAL_ADC_Stop(&hadc2);
  if ((status != HAL_OK) && (result == HAL_OK)) {
    result = status;
  }

  s_running = false;
  return result;
}

HAL_StatusTypeDef DualADC_Start(uint32_t *packed_pairs,
                                uint32_t pair_count,
                                uint32_t requested_rate_hz,
                                uint32_t *actual_rate_hz)
{
  HAL_StatusTypeDef status;

  if ((packed_pairs == NULL) || (pair_count < 2U) ||
      (pair_count > 65535U) ||
      ((((uintptr_t)packed_pairs) & 0x3U) != 0U)) {
    return HAL_ERROR;
  }

  status = DualADC_Stop();
  if (status != HAL_OK) {
    return status;
  }
  if (!DualADC_ConfigureTimer(requested_rate_hz, actual_rate_hz)) {
    return HAL_ERROR;
  }

  __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_EOC | ADC_FLAG_OVR);
  __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_EOC | ADC_FLAG_OVR);

  /* The slave ADC must be enabled before the externally-triggered master. */
  status = HAL_ADC_Start(&hadc2);
  if (status != HAL_OK) {
    return status;
  }

  status = HAL_ADCEx_MultiModeStart_DMA(&hadc1, packed_pairs, pair_count);
  if (status != HAL_OK) {
    (void)HAL_ADC_Stop(&hadc2);
    return status;
  }

  status = HAL_TIM_Base_Start(&htim2);
  if (status != HAL_OK) {
    (void)HAL_ADCEx_MultiModeStop_DMA(&hadc1);
    (void)HAL_ADC_Stop(&hadc2);
    return status;
  }

  s_running = true;
  return HAL_OK;
}

bool DualADC_IsRunning(void)
{
  return s_running;
}
