/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dac.c
  * @brief   DAC configuration for the Task5 sawtooth reference.
  ******************************************************************************
  */
/* USER CODE END Header */
#include "dac.h"

DMA_HandleTypeDef hdma_dac1;

void MX_DAC_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_DAC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* PA4 ------> DAC_OUT1 */
  gpio.Pin = GPIO_PIN_4;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &gpio);

  hdma_dac1.Instance = DMA1_Stream5;
  hdma_dac1.Init.Channel = DMA_CHANNEL_7;
  hdma_dac1.Init.Direction = DMA_MEMORY_TO_PERIPH;
  hdma_dac1.Init.PeriphInc = DMA_PINC_DISABLE;
  hdma_dac1.Init.MemInc = DMA_MINC_ENABLE;
  hdma_dac1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  hdma_dac1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  hdma_dac1.Init.Mode = DMA_CIRCULAR;
  hdma_dac1.Init.Priority = DMA_PRIORITY_HIGH;
  hdma_dac1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&hdma_dac1) != HAL_OK)
  {
    Error_Handler();
  }

  /*
   * Channel 1: TIM6 TRGO trigger (TSEL1=000), trigger enabled, output
   * buffer enabled. EN1 and DMAEN1 stay clear until AppSaw_Start().
   */
  CLEAR_BIT(
      DAC->CR,
      DAC_CR_EN1 | DAC_CR_BOFF1 | DAC_CR_TEN1 |
          DAC_CR_TSEL1 | DAC_CR_WAVE1 |
          DAC_CR_MAMP1 | DAC_CR_DMAEN1);
  SET_BIT(DAC->CR, DAC_CR_TEN1);
  DAC->DHR12R1 = 0U;
}
