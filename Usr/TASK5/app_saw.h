#ifndef APP_SAW_H
#define APP_SAW_H

#include "stm32f4xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SAW_FREQUENCY_1KHZ       (1000UL)
#define APP_SAW_FREQUENCY_10KHZ      (10000UL)
#define APP_SAW_SAMPLE_COUNT          (100U)

typedef struct {
  bool running;
  uint32_t frequency_hz;
  uint32_t sample_rate_hz;
  uint32_t start_error_count;
  uint32_t dma_underrun_count;
} app_saw_status_t;

HAL_StatusTypeDef AppSaw_Init(void);
HAL_StatusTypeDef AppSaw_Start(uint32_t frequency_hz);
void AppSaw_Stop(void);
void AppSaw_Process(void);
void AppSaw_GetStatus(app_saw_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_SAW_H */
