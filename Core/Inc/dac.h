/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dac.h
  * @brief   This file contains all the function prototypes for dac.c.
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __DAC_H__
#define __DAC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

extern DMA_HandleTypeDef hdma_dac1;

void MX_DAC_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* __DAC_H__ */
