#ifndef OPENMV_UART_H
#define OPENMV_UART_H

#include "openmv_protocol.h"
#include "stm32f4xx_hal.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*openmv_uart_frame_handler_t)(
    const openmv_frame_t *frame,
    void *context);

typedef struct {
  uint32_t rx_ring_overflow_count;
  uint32_t rx_restart_error_count;
  uint32_t tx_ring_overflow_count;
  uint32_t tx_start_error_count;
  uint32_t uart_error_count;
  openmv_parser_diagnostics_t parser;
} openmv_uart_diagnostics_t;

HAL_StatusTypeDef OpenMV_UART_Init(
    openmv_uart_frame_handler_t handler,
    void *context);
void OpenMV_UART_Service(void);
bool OpenMV_UART_Send(uint8_t type,
                      uint8_t seq,
                      const uint8_t *payload,
                      uint16_t payload_length);
void OpenMV_UART_GetDiagnostics(
    openmv_uart_diagnostics_t *diagnostics);

/* HAL callback forwarders. Return true only for USART2. */
bool OpenMV_UART_RxCpltCallback(UART_HandleTypeDef *huart);
bool OpenMV_UART_TxCpltCallback(UART_HandleTypeDef *huart);
bool OpenMV_UART_ErrorCallback(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* OPENMV_UART_H */
