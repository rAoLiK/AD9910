#include "app_saw.h"

#include "dac.h"
#include "tim.h"

#include <stddef.h>
#include <string.h>

#define APP_SAW_DAC_MAX_CODE          (4095UL)

static uint16_t s_saw_table[APP_SAW_SAMPLE_COUNT];
static app_saw_status_t s_saw_status;
static bool s_saw_initialized;

static uint32_t AppSaw_TimerClockHz(void)
{
  uint32_t clock_hz = HAL_RCC_GetPCLK1Freq();

  if (HAL_RCC_GetPCLK1Freq() != HAL_RCC_GetHCLKFreq()) {
    clock_hz *= 2UL;
  }
  return clock_hz;
}

static bool AppSaw_ConfigureTimer(uint32_t sample_rate_hz)
{
  uint32_t timer_clock_hz;
  uint32_t ticks;

  if (sample_rate_hz == 0UL) {
    return false;
  }
  timer_clock_hz = AppSaw_TimerClockHz();
  if ((timer_clock_hz % sample_rate_hz) != 0UL) {
    return false;
  }
  ticks = timer_clock_hz / sample_rate_hz;
  if ((ticks == 0UL) || (ticks > 65536UL)) {
    return false;
  }

  __HAL_TIM_DISABLE(&htim6);
  __HAL_TIM_SET_PRESCALER(&htim6, 0UL);
  __HAL_TIM_SET_AUTORELOAD(&htim6, ticks - 1UL);
  __HAL_TIM_SET_COUNTER(&htim6, 0UL);
  htim6.Instance->EGR = TIM_EGR_UG;
  return true;
}

HAL_StatusTypeDef AppSaw_Init(void)
{
  uint32_t i;

  memset(&s_saw_status, 0, sizeof(s_saw_status));
  for (i = 0UL; i < APP_SAW_SAMPLE_COUNT; i++) {
    s_saw_table[i] = (uint16_t)(
        (i * APP_SAW_DAC_MAX_CODE) /
        (APP_SAW_SAMPLE_COUNT - 1U));
  }
  if ((s_saw_table[0] != 0U) ||
      (s_saw_table[APP_SAW_SAMPLE_COUNT - 1U] !=
       APP_SAW_DAC_MAX_CODE)) {
    return HAL_ERROR;
  }
  s_saw_initialized = true;
  AppSaw_Stop();
  return HAL_OK;
}

HAL_StatusTypeDef AppSaw_Start(uint32_t frequency_hz)
{
  uint32_t sample_rate_hz;
  HAL_StatusTypeDef dma_result;

  if (!s_saw_initialized ||
      ((frequency_hz != APP_SAW_FREQUENCY_1KHZ) &&
       (frequency_hz != APP_SAW_FREQUENCY_10KHZ))) {
    s_saw_status.start_error_count++;
    return HAL_ERROR;
  }

  sample_rate_hz = frequency_hz * APP_SAW_SAMPLE_COUNT;
  AppSaw_Stop();
  if (!AppSaw_ConfigureTimer(sample_rate_hz)) {
    s_saw_status.start_error_count++;
    return HAL_ERROR;
  }

  DAC->SR = DAC_SR_DMAUDR1;
  dma_result = HAL_DMA_Start(
      &hdma_dac1,
      (uint32_t)s_saw_table,
      (uint32_t)&DAC->DHR12R1,
      APP_SAW_SAMPLE_COUNT);
  if (dma_result != HAL_OK) {
    s_saw_status.start_error_count++;
    return dma_result;
  }

  SET_BIT(DAC->CR, DAC_CR_DMAEN1 | DAC_CR_EN1);
  if (HAL_TIM_Base_Start(&htim6) != HAL_OK) {
    CLEAR_BIT(DAC->CR, DAC_CR_DMAEN1 | DAC_CR_EN1);
    (void)HAL_DMA_Abort(&hdma_dac1);
    s_saw_status.start_error_count++;
    return HAL_ERROR;
  }

  s_saw_status.running = true;
  s_saw_status.frequency_hz = frequency_hz;
  s_saw_status.sample_rate_hz = sample_rate_hz;
  return HAL_OK;
}

void AppSaw_Stop(void)
{
  (void)HAL_TIM_Base_Stop(&htim6);
  CLEAR_BIT(DAC->CR, DAC_CR_DMAEN1 | DAC_CR_EN1);
  (void)HAL_DMA_Abort(&hdma_dac1);
  DAC->DHR12R1 = 0U;
  s_saw_status.running = false;
  s_saw_status.frequency_hz = 0UL;
  s_saw_status.sample_rate_hz = 0UL;
}

void AppSaw_Process(void)
{
  if (s_saw_status.running &&
      ((DAC->SR & DAC_SR_DMAUDR1) != 0UL)) {
    DAC->SR = DAC_SR_DMAUDR1;
    s_saw_status.dma_underrun_count++;
  }
}

void AppSaw_GetStatus(app_saw_status_t *status)
{
  if (status == NULL) {
    return;
  }
  *status = s_saw_status;
}
