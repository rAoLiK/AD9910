#include "openmv_uart.h"

#include "usart.h"

#include <string.h>

#define OPENMV_UART_RX_RING_SIZE     (512U)
#define OPENMV_UART_RX_RING_MASK     (OPENMV_UART_RX_RING_SIZE - 1U)
#define OPENMV_UART_TX_RING_SIZE     (512U)
#define OPENMV_UART_TX_RING_MASK     (OPENMV_UART_TX_RING_SIZE - 1U)
#define OPENMV_UART_SERVICE_BUDGET   (512U)

typedef struct {
  uint8_t rx_ring[OPENMV_UART_RX_RING_SIZE];
  uint8_t tx_ring[OPENMV_UART_TX_RING_SIZE];
  volatile uint16_t rx_head;
  volatile uint16_t rx_tail;
  volatile uint16_t tx_head;
  volatile uint16_t tx_tail;
  uint8_t rx_byte;
  uint8_t tx_byte;
  volatile bool tx_active;
  bool initialized;
  openmv_parser_t parser;
  openmv_uart_frame_handler_t handler;
  void *handler_context;
  openmv_uart_diagnostics_t diagnostics;
} openmv_uart_context_t;

static openmv_uart_context_t s_openmv_uart;

static uint32_t OpenMV_UART_EnterCritical(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  __DMB();
  return primask;
}

static void OpenMV_UART_ExitCritical(uint32_t primask)
{
  __DMB();
  __set_PRIMASK(primask);
}

static void OpenMV_UART_KickTx(void)
{
  uint32_t primask;
  bool start = false;

  primask = OpenMV_UART_EnterCritical();
  if (!s_openmv_uart.tx_active &&
      (s_openmv_uart.tx_tail != s_openmv_uart.tx_head)) {
    s_openmv_uart.tx_byte =
        s_openmv_uart.tx_ring[s_openmv_uart.tx_tail];
    s_openmv_uart.tx_active = true;
    start = true;
  }
  OpenMV_UART_ExitCritical(primask);

  if (start &&
      (HAL_UART_Transmit_IT(
           &huart5, &s_openmv_uart.tx_byte, 1U) != HAL_OK)) {
    primask = OpenMV_UART_EnterCritical();
    s_openmv_uart.tx_active = false;
    s_openmv_uart.diagnostics.tx_start_error_count++;
    OpenMV_UART_ExitCritical(primask);
  }
}

HAL_StatusTypeDef OpenMV_UART_Init(
    openmv_uart_frame_handler_t handler,
    void *context)
{
  memset(&s_openmv_uart, 0, sizeof(s_openmv_uart));
  OpenMV_Parser_Init(&s_openmv_uart.parser);
  s_openmv_uart.handler = handler;
  s_openmv_uart.handler_context = context;

  if (HAL_UART_Receive_IT(
          &huart5, &s_openmv_uart.rx_byte, 1U) != HAL_OK) {
    s_openmv_uart.diagnostics.rx_restart_error_count++;
    return HAL_ERROR;
  }
  s_openmv_uart.initialized = true;
  return HAL_OK;
}

void OpenMV_UART_Service(void)
{
  openmv_frame_t frame;
  uint32_t processed = 0U;

  if (!s_openmv_uart.initialized) {
    return;
  }

  while ((processed < OPENMV_UART_SERVICE_BUDGET) &&
         (s_openmv_uart.rx_tail != s_openmv_uart.rx_head)) {
    uint8_t byte =
        s_openmv_uart.rx_ring[s_openmv_uart.rx_tail];
    s_openmv_uart.rx_tail =
        (uint16_t)((s_openmv_uart.rx_tail + 1U) &
                   OPENMV_UART_RX_RING_MASK);
    processed++;

    if (OpenMV_Parser_Feed(
            &s_openmv_uart.parser, byte, &frame) &&
        (s_openmv_uart.handler != NULL)) {
      s_openmv_uart.handler(
          &frame, s_openmv_uart.handler_context);
    }
  }

  OpenMV_UART_KickTx();
}

bool OpenMV_UART_Send(uint8_t type,
                      uint8_t seq,
                      const uint8_t *payload,
                      uint16_t payload_length)
{
  uint8_t encoded[OPENMV_PROTOCOL_MAX_FRAME_SIZE];
  size_t encoded_length;
  uint16_t used;
  uint16_t free_bytes;
  uint32_t primask;
  size_t i;

  if (!s_openmv_uart.initialized ||
      !OpenMV_EncodeFrame(type, seq, payload, payload_length,
                          encoded, sizeof(encoded),
                          &encoded_length)) {
    return false;
  }

  primask = OpenMV_UART_EnterCritical();
  used = (uint16_t)((s_openmv_uart.tx_head -
                     s_openmv_uart.tx_tail) &
                    OPENMV_UART_TX_RING_MASK);
  free_bytes = (uint16_t)(
      (OPENMV_UART_TX_RING_SIZE - 1U) - used);
  if (encoded_length > free_bytes) {
    s_openmv_uart.diagnostics.tx_ring_overflow_count++;
    OpenMV_UART_ExitCritical(primask);
    return false;
  }

  for (i = 0U; i < encoded_length; i++) {
    s_openmv_uart.tx_ring[s_openmv_uart.tx_head] = encoded[i];
    s_openmv_uart.tx_head =
        (uint16_t)((s_openmv_uart.tx_head + 1U) &
                   OPENMV_UART_TX_RING_MASK);
  }
  OpenMV_UART_ExitCritical(primask);
  OpenMV_UART_KickTx();
  return true;
}

void OpenMV_UART_GetDiagnostics(
    openmv_uart_diagnostics_t *diagnostics)
{
  uint32_t primask;

  if (diagnostics == NULL) {
    return;
  }
  primask = OpenMV_UART_EnterCritical();
  *diagnostics = s_openmv_uart.diagnostics;
  OpenMV_UART_ExitCritical(primask);
  OpenMV_Parser_GetDiagnostics(
      &s_openmv_uart.parser, &diagnostics->parser);
}

bool OpenMV_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  uint16_t next;

  if ((huart == NULL) || (huart->Instance != UART5)) {
    return false;
  }

  next = (uint16_t)((s_openmv_uart.rx_head + 1U) &
                    OPENMV_UART_RX_RING_MASK);
  if (next == s_openmv_uart.rx_tail) {
    s_openmv_uart.diagnostics.rx_ring_overflow_count++;
  } else {
    s_openmv_uart.rx_ring[s_openmv_uart.rx_head] =
        s_openmv_uart.rx_byte;
    s_openmv_uart.rx_head = next;
    __DMB();
  }

  if (HAL_UART_Receive_IT(
          &huart5, &s_openmv_uart.rx_byte, 1U) != HAL_OK) {
    s_openmv_uart.diagnostics.rx_restart_error_count++;
  }
  return true;
}

bool OpenMV_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if ((huart == NULL) || (huart->Instance != UART5)) {
    return false;
  }

  if (s_openmv_uart.tx_active &&
      (s_openmv_uart.tx_tail != s_openmv_uart.tx_head)) {
    s_openmv_uart.tx_tail =
        (uint16_t)((s_openmv_uart.tx_tail + 1U) &
                   OPENMV_UART_TX_RING_MASK);
  }
  s_openmv_uart.tx_active = false;
  __DMB();
  OpenMV_UART_KickTx();
  return true;
}

bool OpenMV_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if ((huart == NULL) || (huart->Instance != UART5)) {
    return false;
  }

  s_openmv_uart.diagnostics.uart_error_count++;
  s_openmv_uart.tx_active = false;
  (void)HAL_UART_AbortReceive(huart);
  if (HAL_UART_Receive_IT(
          &huart5, &s_openmv_uart.rx_byte, 1U) != HAL_OK) {
    s_openmv_uart.diagnostics.rx_restart_error_count++;
  }
  OpenMV_UART_KickTx();
  return true;
}
