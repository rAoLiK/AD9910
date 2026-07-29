#ifndef __TJC_USART_HMI_H__
#define __TJC_USART_HMI_H__

#include "main.h"
#include "tjc_protocol_parser.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief  串口屏通信使用的 UART 句柄。
 */
#ifndef TJC_UART_HANDLE
#define TJC_UART_HANDLE huart3
#endif

/**
 * @brief  串口屏通信使用的 UART 实例。
 */
#ifndef TJC_UART_INSTANCE
#define TJC_UART_INSTANCE USART3
#endif

/**
 * @brief  接收环形缓冲区长度。
 */
#ifndef TJC_RINGBUFFER_LEN
#define TJC_RINGBUFFER_LEN 512U
#endif

/**
 * @brief  单次接收缓冲长度。
 */
#ifndef TJC_RX_CHUNK_SIZE
#define TJC_RX_CHUNK_SIZE 1U
#endif

/**
 * @brief  协议帧长度（55 + cmd + p1 + p2 + FF FF FF）。
 */
#ifndef TJC_FRAME_LENGTH
#define TJC_FRAME_LENGTH 7U
#endif

/**
 * @brief  协议帧头。
 */
#ifndef TJC_FRAME_HEADER
#define TJC_FRAME_HEADER 0x55U
#endif

/**
 * @brief  协议帧尾字节。
 */
#ifndef TJC_FRAME_END_BYTE
#define TJC_FRAME_END_BYTE 0xFFU
#endif

/**
 * @brief  发送超时时间（毫秒）。
 */
#ifndef TJC_TX_TIMEOUT_MS
#define TJC_TX_TIMEOUT_MS 20U
#endif

/**
 * @brief  异步发送等待超时时间（毫秒）。
 */
#ifndef TJC_TX_WAIT_TIMEOUT_MS
#define TJC_TX_WAIT_TIMEOUT_MS 200U
#endif

/**
 * @brief  中断上下文轮询发送时的最大等待循环次数。
 */
#ifndef TJC_ISR_TX_TIMEOUT_LOOPS
#define TJC_ISR_TX_TIMEOUT_LOOPS 1000000UL
#endif

/**
 * @brief  单次拼包发送的最大缓存长度。
 */
#ifndef TJC_TX_PACKET_MAX_LEN
#define TJC_TX_PACKET_MAX_LEN 256U
#endif

/** Non-blocking TX byte ring. Must be a power of two. */
#ifndef TJC_TX_RINGBUFFER_LEN
#define TJC_TX_RINGBUFFER_LEN 2048U
#endif

/**
 * @brief  串口传输模式枚举值：阻塞模式。
 */
#define TJC_UART_MODE_BLOCKING 0U

/**
 * @brief  串口传输模式枚举值：中断模式。
 */
#define TJC_UART_MODE_IT 1U

/**
 * @brief  串口传输模式枚举值：DMA 模式。
 */
#define TJC_UART_MODE_DMA 2U

/**
 * @brief  RX 模式配置，默认逐字节中断。
 */
#ifndef TJC_UART_RX_MODE
#define TJC_UART_RX_MODE TJC_UART_MODE_IT
#endif

/**
 * @brief  TX 模式配置，默认中断发送。
 */
#ifndef TJC_UART_TX_MODE
#define TJC_UART_TX_MODE TJC_UART_MODE_IT
#endif

#if (TJC_RX_CHUNK_SIZE == 0U)
#error "TJC_RX_CHUNK_SIZE must be greater than 0"
#endif

#if (TJC_RINGBUFFER_LEN < TJC_FRAME_LENGTH)
#error "TJC_RINGBUFFER_LEN must be greater than or equal to TJC_FRAME_LENGTH"
#endif

#if ((TJC_TX_RINGBUFFER_LEN < TJC_TX_PACKET_MAX_LEN) || \
     ((TJC_TX_RINGBUFFER_LEN & (TJC_TX_RINGBUFFER_LEN - 1U)) != 0U))
#error "TJC_TX_RINGBUFFER_LEN must be a power of two and fit one packet"
#endif

#if ((TJC_UART_RX_MODE != TJC_UART_MODE_IT) && (TJC_UART_RX_MODE != TJC_UART_MODE_DMA))
#error "TJC_UART_RX_MODE must be TJC_UART_MODE_IT or TJC_UART_MODE_DMA"
#endif

#if ((TJC_UART_TX_MODE != TJC_UART_MODE_BLOCKING) && (TJC_UART_TX_MODE != TJC_UART_MODE_IT) && (TJC_UART_TX_MODE != TJC_UART_MODE_DMA))
#error "TJC_UART_TX_MODE must be BLOCKING / IT / DMA"
#endif

#if (TJC_UART_RX_MODE == TJC_UART_MODE_IT)
#define TJC_UART_START_RX(huart, pData, size) HAL_UART_Receive_IT((huart), (pData), (size))
#elif (TJC_UART_RX_MODE == TJC_UART_MODE_DMA)
#define TJC_UART_START_RX(huart, pData, size) HAL_UART_Receive_DMA((huart), (pData), (size))
#endif

#if (TJC_UART_TX_MODE == TJC_UART_MODE_BLOCKING)
#define TJC_UART_START_TX(huart, pData, size) HAL_UART_Transmit((huart), (pData), (size), TJC_TX_TIMEOUT_MS)
#elif (TJC_UART_TX_MODE == TJC_UART_MODE_IT)
#define TJC_UART_START_TX(huart, pData, size) HAL_UART_Transmit_IT((huart), (pData), (size))
#elif (TJC_UART_TX_MODE == TJC_UART_MODE_DMA)
#define TJC_UART_START_TX(huart, pData, size) HAL_UART_Transmit_DMA((huart), (pData), (size))
#endif

extern UART_HandleTypeDef TJC_UART_HANDLE;

/**
 * @brief  协议消息回调函数类型。
 */
typedef void (*TJC_MessageHandler_t)(const TJC_ProtocolMessage_t *message);

typedef struct {
  uint32_t rx_overflow_count;
  uint32_t rx_restart_count;
  uint32_t tx_overflow_count;
  uint32_t tx_error_count;
  uint32_t isr_send_reject_count;
} TJC_Diagnostics_t;

/**
 * @brief  初始化驱动并启动接收。
 */
HAL_StatusTypeDef TJC_Init(void);

/** Parse RX data and retry pending non-blocking transport work. */
void TJC_Service(void);

/**
 * @brief  按当前宏配置启动接收。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_StartReceive(void);

/**
 * @brief  串口接收完成转发函数。
 * @param  huart 触发中断的 UART 句柄。
 */
void TJC_UART_RxCpltCallback(UART_HandleTypeDef *huart);

/** Forward HAL TX-complete callbacks; this only advances the TX ring. */
void TJC_UART_TxCpltCallback(UART_HandleTypeDef *huart);

/**
 * @brief  串口异常回调转发函数。
 * @param  huart 触发异常的 UART 句柄。
 */
void TJC_UART_ErrorCallback(UART_HandleTypeDef *huart);

/**
 * @brief  解析接收缓冲区中的协议帧。
 */
void TJC_ParseIncomingData(void);

/**
 * @brief  注册协议消息回调。
 * @param  handler 用户自定义处理函数，传 NULL 则取消注册。
 */
void TJC_RegisterMessageHandler(TJC_MessageHandler_t handler);

/**
 * @brief  读取最近一次成功解析的协议消息。
 * @param  message 输出消息缓存。
 * @retval true 读取成功，false 当前没有新消息。
 */
bool TJC_GetLastMessage(TJC_ProtocolMessage_t *message);

void TJC_GetDiagnostics(TJC_Diagnostics_t *diagnostics);

/**
 * @brief  发送字符串并自动追加结束符。
 * @param  str 待发送字符串。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendString(const char *str);

/**
 * @brief  发送原始字符串。
 * @param  str 待发送字符串。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendRawString(const char *str);

/**
 * @brief  发送文本控件属性。
 * @param  objname 控件名称。
 * @param  attribute 属性名。
 * @param  txt 文本内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendText(const char *objname, const char *attribute, const char *txt);

/**
 * @brief  发送数值控件属性。
 * @param  objname 控件名称。
 * @param  attribute 属性名。
 * @param  val 数值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendValue(const char *objname, const char *attribute, int32_t val);

/**
 * @brief  发送指定长度字节流。
 * @param  data 数据指针。
 * @param  len 字节长度。
 * @param  append_terminator 是否追加协议结束符。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SendBytes(const uint8_t *data, uint16_t len, bool append_terminator);

/**
 * @brief  清空环形缓冲区。
 */
void TJC_RingBufferReset(void);

/**
 * @brief  获取环形缓冲区当前数据长度。
 * @retval 数据长度。
 */
uint16_t TJC_RingBufferLength(void);

/**
 * @brief  读取环形缓冲区指定偏移字节。
 * @param  position 相对头部偏移。
 * @retval 读取到的字节。
 */
uint8_t TJC_RingBufferPeek(uint16_t position);

/**
 * @brief  删除环形缓冲区头部指定长度的数据。
 * @param  size 删除长度。
 */
void TJC_RingBufferDrop(uint16_t size);

#endif
