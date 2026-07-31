#ifndef APP_INTEGRATION_H
#define APP_INTEGRATION_H

#include "ad9910.h"
#include "app_core.h"
#include "app_saw.h"
#include "openmv_uart.h"
#include "stm32f4xx_hal.h"
#include "task5_controller.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  app_status_t app;
  uint32_t command_queue_overflow_count;
  uint32_t hmi_execute_error_count;
  uint32_t hmi_unknown_message_count;
  task5_status_t task5;
  app_saw_status_t saw;
  openmv_uart_diagnostics_t openmv_uart;
} app_integration_status_t;

HAL_StatusTypeDef AppIntegration_Init(ad9910_t *dds);
void AppIntegration_Process(void);
void AppIntegration_GetStatus(app_integration_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* APP_INTEGRATION_H */
