#ifndef APP_INTEGRATION_H
#define APP_INTEGRATION_H

#include "app_core.h"
#include "stm32f4xx_hal.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  app_status_t app;
  uint32_t command_queue_overflow_count;
  uint32_t hmi_execute_error_count;
  uint32_t hmi_unknown_message_count;
} app_integration_status_t;

HAL_StatusTypeDef AppIntegration_Init(void);
void AppIntegration_Process(void);
void AppIntegration_GetStatus(app_integration_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_INTEGRATION_H */
