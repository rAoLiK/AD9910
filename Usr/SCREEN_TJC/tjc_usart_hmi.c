#include "tjc_usart_hmi.h"

#include <stdio.h>
#include <string.h>

extern UART_HandleTypeDef TJC_UART_HANDLE;

typedef struct {
  volatile uint16_t head;
  volatile uint16_t tail;
  volatile uint16_t length;
  uint8_t data[TJC_RINGBUFFER_LEN];
} TJC_RingBuffer_t;

static TJC_RingBuffer_t s_ring = {0};
static uint8_t s_rxBuffer[TJC_RX_CHUNK_SIZE] = {0};
static TJC_MessageHandler_t s_messageHandler = NULL;
static TJC_ProtocolMessage_t s_lastMessage;
static volatile uint8_t s_hasLastMessage = 0U;
static volatile uint8_t s_rxRestartNeeded = 0U;
static volatile uint32_t s_localOverflowCount = 0U;

/**
 * @brief  进入临界区。
 */
#define TJC_ENTER_CRITICAL(state) do { \
  (state) = __get_PRIMASK(); \
  __disable_irq(); \
  __DMB(); \
} while (0)

/**
 * @brief  退出临界区。
 */
#define TJC_EXIT_CRITICAL(state) do { \
  __DMB(); \
  __set_PRIMASK(state); \
} while (0)

/**
 * @brief  向环形缓冲区写入 1 字节数据。
 * @param  byte 待写入字节。
 */
static void TJC_RingBufferWriteByte(uint8_t byte)
{
  uint32_t primask;

  TJC_ENTER_CRITICAL(primask);
  if (s_ring.length < TJC_RINGBUFFER_LEN) {
    s_ring.data[s_ring.tail] = byte;
    s_ring.tail = (uint16_t)((s_ring.tail + 1U) % TJC_RINGBUFFER_LEN);
    s_ring.length++;
  } else {
    s_localOverflowCount++;
  }
  TJC_EXIT_CRITICAL(primask);
}

/**
 * @brief  判断当前是否处于中断上下文。
 * @retval true 当前处于中断上下文，false 当前处于线程上下文。
 */
static bool TJC_IsInInterruptContext(void)
{
  return (__get_IPSR() != 0U);
}

/**
 * @brief  在中断上下文中等待指定串口标志位到达目标状态。
 * @param  flag 需要等待的标志位。
 * @param  expectedState 期望状态。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_WaitFlagInIsr(uint32_t flag, FlagStatus expectedState)
{
  uint32_t loopCount = TJC_ISR_TX_TIMEOUT_LOOPS;

  while (__HAL_UART_GET_FLAG(&TJC_UART_HANDLE, flag) != expectedState) {
    if (loopCount == 0UL) {
      return HAL_TIMEOUT;
    }
    loopCount--;
  }

  return HAL_OK;
}

/**
 * @brief  在中断上下文中使用轮询方式直接发送数据。
 * @param  data 数据指针。
 * @param  len 数据长度。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_TransmitDirectInIsr(const uint8_t *data, uint16_t len)
{
  uint16_t index;
  HAL_StatusTypeDef status;

  if ((data == NULL) || (len == 0U)) {
    return HAL_ERROR;
  }

  for (index = 0U; index < len; index++) {
    status = TJC_WaitFlagInIsr(UART_FLAG_TXE, SET);
    if (status != HAL_OK) {
      return status;
    }

    TJC_UART_HANDLE.Instance->DR = data[index];
  }

  return TJC_WaitFlagInIsr(UART_FLAG_TC, SET);
}

/**
 * @brief  等待异步发送结束。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_WaitForTxComplete(void)
{
#if (TJC_UART_TX_MODE == TJC_UART_MODE_BLOCKING)
  return HAL_OK;
#else
  if (TJC_IsInInterruptContext()) {
    return HAL_OK;
  }

  uint32_t startTick = HAL_GetTick();
  HAL_UART_StateTypeDef txState;

  do {
    /*
     * HAL_UART_GetState() ORs gState with RxState.  RX DMA is continuously
     * active in this driver, so that combined value remains BUSY_RX even
     * after TX has completed.  HAL_UART_StateTypeDef values are encoded
     * states, not independent bit flags; testing them with bitwise AND makes
     * READY (0x20) look busy against BUSY_TX (0x21).  gState is the HAL field
     * that exclusively owns TX progress.
     */
    txState = TJC_UART_HANDLE.gState;
    if (txState == HAL_UART_STATE_READY) {
      return HAL_OK;
    }
    if ((txState == HAL_UART_STATE_ERROR) ||
        (txState == HAL_UART_STATE_TIMEOUT) ||
        (txState == HAL_UART_STATE_RESET)) {
      return HAL_ERROR;
    }
  } while ((HAL_GetTick() - startTick) < TJC_TX_WAIT_TIMEOUT_MS);
  return HAL_TIMEOUT;

#endif
}

/**
 * @brief  根据宏配置启动一次接收。
 * @param  huart UART 句柄。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_StartReceiveInternal(UART_HandleTypeDef *huart)
{
  HAL_StatusTypeDef status;

  status = TJC_UART_START_RX(huart, s_rxBuffer, TJC_RX_CHUNK_SIZE);
  if (status != HAL_OK) {
    s_rxRestartNeeded = 1U;
  }

  return status;
}

/**
 * @brief  根据宏配置启动一次发送。
 * @param  data 数据指针。
 * @param  len 发送长度。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_StartTransmitInternal(const uint8_t *data, uint16_t len)
{
  HAL_StatusTypeDef status;

  if (TJC_IsInInterruptContext()) {
    return TJC_TransmitDirectInIsr(data, len);
  }

  status = TJC_UART_START_TX(&TJC_UART_HANDLE, (uint8_t *)data, len);
  if (status != HAL_OK) {
    return status;
  }

  return TJC_WaitForTxComplete();
}

/**
 * @brief  发送协议结束符。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_SendTerminator(void)
{
  static const uint8_t endBytes[3] = {
    TJC_FRAME_END_BYTE,
    TJC_FRAME_END_BYTE,
    TJC_FRAME_END_BYTE
  };

  return TJC_StartTransmitInternal(endBytes, 3U);
}

/**
 * @brief  发送正文与可选结束符，优先合并为一次完整发送。
 * @param  data 正文数据指针。
 * @param  len 正文长度。
 * @param  appendTerminator 是否追加结束符。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_SendPacket(const uint8_t *data, uint16_t len, bool appendTerminator)
{
  uint8_t packet[TJC_TX_PACKET_MAX_LEN];
  uint16_t totalLen;

  if ((data == NULL) || (len == 0U)) {
    return HAL_ERROR;
  }

  if (!appendTerminator) {
    return TJC_StartTransmitInternal(data, len);
  }

  totalLen = (uint16_t)(len + 3U);
  if (totalLen <= TJC_TX_PACKET_MAX_LEN) {
    memcpy(packet, data, len);
    packet[len] = TJC_FRAME_END_BYTE;
    packet[(uint16_t)(len + 1U)] = TJC_FRAME_END_BYTE;
    packet[(uint16_t)(len + 2U)] = TJC_FRAME_END_BYTE;
    return TJC_StartTransmitInternal(packet, totalLen);
  }

  if (TJC_IsInInterruptContext()) {
    HAL_StatusTypeDef status;

    status = TJC_TransmitDirectInIsr(data, len);
    if (status != HAL_OK) {
      return status;
    }

    return TJC_SendTerminator();
  }

  {
    HAL_StatusTypeDef status;

    status = TJC_StartTransmitInternal(data, len);
    if (status != HAL_OK) {
      return status;
    }

    return TJC_SendTerminator();
  }
}

/**
 * @brief  保存最近一条消息并分发回调。
 * @param  message 已成功解析的协议消息。
 */
static void TJC_DispatchMessage(const TJC_ProtocolMessage_t *message)
{
  uint32_t primask;

  if (message == NULL) {
    return;
  }

  TJC_ENTER_CRITICAL(primask);
  s_lastMessage = *message;
  s_hasLastMessage = 1U;
  TJC_EXIT_CRITICAL(primask);

  if (s_messageHandler != NULL) {
    s_messageHandler(message);
  }
}

/**
 * @brief  初始化驱动并启动接收。
 */
void TJC_Init(void)
{
  TJC_RingBufferReset();
  s_hasLastMessage = 0U;
  s_rxRestartNeeded = 0U;
  s_localOverflowCount = 0U;
  (void)TJC_StartReceive();
}

/**
 * @brief  按当前宏配置启动接收。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_StartReceive(void)
{
  return TJC_StartReceiveInternal(&TJC_UART_HANDLE);
}

/**
 * @brief  串口接收完成转发函数。
 * @param  huart 触发中断的 UART 句柄。
 */
void TJC_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  uint16_t index;

  if ((huart == NULL) || (huart->Instance != TJC_UART_INSTANCE)) {
    return;
  }

  for (index = 0U; index < TJC_RX_CHUNK_SIZE; index++) {
    TJC_RingBufferWriteByte(s_rxBuffer[index]);
  }

  (void)TJC_StartReceiveInternal(huart);
}

/**
 * @brief  串口异常回调转发函数。
 * @param  huart 触发异常的 UART 句柄。
 */
void TJC_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if ((huart == NULL) || (huart->Instance != TJC_UART_INSTANCE)) {
    return;
  }

  s_rxRestartNeeded = 1U;
}

/**
 * @brief  解析接收缓冲区中的协议帧。
 */
void TJC_ParseIncomingData(void)
{
  TJC_ProtocolMessage_t message;
  TJC_ProtocolParseResult_t result;
  uint16_t consumedBytes;

  if (s_rxRestartNeeded != 0U) {
    s_rxRestartNeeded = 0U;
    if (TJC_StartReceive() != HAL_OK) {
      s_rxRestartNeeded = 1U;
    }
  }

  while (TJC_RingBufferLength() >= 4U) {
    result = TJC_ProtocolTryParse(TJC_RingBufferLength(),
                                  TJC_RingBufferPeek,
                                  &message,
                                  &consumedBytes);

    if (result == TJC_PROTOCOL_PARSE_NEED_MORE_DATA) {
      break;
    }

    if (result == TJC_PROTOCOL_PARSE_INVALID_PARAM) {
      break;
    }

    if (consumedBytes == 0U) {
      break;
    }

    TJC_RingBufferDrop(consumedBytes);

    if (result == TJC_PROTOCOL_PARSE_OK) {
      TJC_DispatchMessage(&message);
    }
  }
}

/**
 * @brief  注册协议消息回调。
 * @param  handler 用户自定义处理函数。
 */
void TJC_RegisterMessageHandler(TJC_MessageHandler_t handler)
{
  s_messageHandler = handler;
}

/**
 * @brief  读取最近一次成功解析的协议消息。
 * @param  message 输出消息缓存。
 * @retval true 读取成功，false 当前没有新消息。
 */
bool TJC_GetLastMessage(TJC_ProtocolMessage_t *message)
{
  uint32_t primask;

  if ((message == NULL) || (s_hasLastMessage == 0U)) {
    return false;
  }

  TJC_ENTER_CRITICAL(primask);
  *message = s_lastMessage;
  s_hasLastMessage = 0U;
  TJC_EXIT_CRITICAL(primask);

  return true;
}

/**
 * @brief  发送字符串并自动追加结束符。
 * @param  str 待发送字符串。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendString(const char *str)
{
  if (str == NULL) {
    return HAL_ERROR;
  }

  return TJC_SendPacket((const uint8_t *)str, (uint16_t)strlen(str), true);
}

/**
 * @brief  发送原始字符串。
 * @param  str 待发送字符串。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendRawString(const char *str)
{
  if (str == NULL) {
    return HAL_ERROR;
  }

  return TJC_SendPacket((const uint8_t *)str, (uint16_t)strlen(str), false);
}

/**
 * @brief  发送文本控件属性。
 * @param  objname 控件名称。
 * @param  attribute 属性名。
 * @param  txt 文本内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendText(const char *objname, const char *attribute, const char *txt)
{
  char command[128] = {0};
  int length;

  if ((objname == NULL) || (attribute == NULL) || (txt == NULL)) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "%s.%s=\"%s\"", objname, attribute, txt);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_SendString(command);
}

/**
 * @brief  发送数值控件属性。
 * @param  objname 控件名称。
 * @param  attribute 属性名。
 * @param  val 数值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendValue(const char *objname, const char *attribute, int32_t val)
{
  char command[64] = {0};
  int length;

  if ((objname == NULL) || (attribute == NULL)) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "%s.%s=%ld", objname, attribute, (long)val);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_SendString(command);
}

/**
 * @brief  发送指定长度字节流。
 * @param  data 数据指针。
 * @param  len 字节长度。
 * @param  append_terminator 是否追加协议结束符。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendBytes(const uint8_t *data, uint16_t len, bool append_terminator)
{
  if ((data == NULL) || (len == 0U)) {
    return HAL_ERROR;
  }

  return TJC_SendPacket(data, len, append_terminator);
}

/**
 * @brief  清空环形缓冲区。
 */
void TJC_RingBufferReset(void)
{
  uint32_t primask;

  TJC_ENTER_CRITICAL(primask);
  s_ring.head = 0U;
  s_ring.tail = 0U;
  s_ring.length = 0U;
  TJC_EXIT_CRITICAL(primask);
}

/**
 * @brief  获取环形缓冲区当前数据长度。
 * @retval 数据长度。
 */
uint16_t TJC_RingBufferLength(void)
{
  return s_ring.length;
}

/**
 * @brief  读取环形缓冲区指定偏移字节。
 * @param  position 相对头部偏移。
 * @retval 读取到的字节。
 */
uint8_t TJC_RingBufferPeek(uint16_t position)
{
  uint16_t realPosition;

  if (position >= s_ring.length) {
    return 0U;
  }

  realPosition = (uint16_t)((s_ring.head + position) % TJC_RINGBUFFER_LEN);
  return s_ring.data[realPosition];
}

/**
 * @brief  删除环形缓冲区头部指定长度的数据。
 * @param  size 删除长度。
 */
void TJC_RingBufferDrop(uint16_t size)
{
  uint16_t dropCount;
  uint32_t primask;

  if (size >= s_ring.length) {
    TJC_RingBufferReset();
    return;
  }

  TJC_ENTER_CRITICAL(primask);
  dropCount = size;
  while (dropCount > 0U) {
    s_ring.head = (uint16_t)((s_ring.head + 1U) % TJC_RINGBUFFER_LEN);
    s_ring.length--;
    dropCount--;
  }
  TJC_EXIT_CRITICAL(primask);
}
