#include "adc.h"

ADC_HandleTypeDef hadc1;
ADC_HandleTypeDef hadc2;
DMA_HandleTypeDef hdma_adc1;

void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef channel = {0};
  ADC_MultiModeTypeDef multimode = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1U;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) {
    Error_Handler();
  }

  channel.Channel = ADC_CHANNEL_10;
  channel.Rank = 1U;
  channel.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  channel.Offset = 0U;
  if (HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK) {
    Error_Handler();
  }

  multimode.Mode = ADC_DUALMODE_REGSIMULT;
  multimode.DMAAccessMode = ADC_DMAACCESSMODE_2;
  multimode.TwoSamplingDelay = ADC_TWOSAMPLINGDELAY_5CYCLES;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK) {
    Error_Handler();
  }
}

void MX_ADC2_Init(void)
{
  ADC_ChannelConfTypeDef channel = {0};

  hadc2.Instance = ADC2;
  hadc2.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.ScanConvMode = DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.NbrOfConversion = 1U;
  hadc2.Init.DMAContinuousRequests = DISABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc2) != HAL_OK) {
    Error_Handler();
  }

  channel.Channel = ADC_CHANNEL_11;
  channel.Rank = 1U;
  channel.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  channel.Offset = 0U;
  if (HAL_ADC_ConfigChannel(&hadc2, &channel) != HAL_OK) {
    Error_Handler();
  }
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *handle)
{
  GPIO_InitTypeDef gpio = {0};

  if ((handle != NULL) && (handle->Instance == ADC1)) {
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC0 -> ADC1_IN10, signal generator reference. */
    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &gpio);

    hdma_adc1.Instance = DMA2_Stream0;
    hdma_adc1.Init.Channel = DMA_CHANNEL_0;
    hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
    hdma_adc1.Init.Mode = DMA_CIRCULAR;
    hdma_adc1.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    hdma_adc1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK) {
      Error_Handler();
    }
    __HAL_LINKDMA(handle, DMA_Handle, hdma_adc1);

    HAL_NVIC_SetPriority(ADC_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(ADC_IRQn);
  } else if ((handle != NULL) && (handle->Instance == ADC2)) {
    __HAL_RCC_ADC2_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* PC1 -> ADC2_IN11, sampled DDS feedback. */
    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &gpio);
  }
}

void HAL_ADC_MspDeInit(ADC_HandleTypeDef *handle)
{
  if ((handle != NULL) && (handle->Instance == ADC1)) {
    __HAL_RCC_ADC1_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_0);
    HAL_DMA_DeInit(handle->DMA_Handle);
    HAL_NVIC_DisableIRQ(ADC_IRQn);
  } else if ((handle != NULL) && (handle->Instance == ADC2)) {
    __HAL_RCC_ADC2_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1);
  }
}
