#ifndef __TJC_PROTOCOL_PARSER_H__
#define __TJC_PROTOCOL_PARSER_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief  协议结束符长度。
 */
#define TJC_PROTOCOL_END_LEN 3U

/**
 * @brief  协议结束符字节。
 */
#define TJC_PROTOCOL_END_BYTE 0xFFU

/**
 * @brief  最大保存的字符串返回长度。
 */
#ifndef TJC_PROTOCOL_STRING_MAX_LEN
#define TJC_PROTOCOL_STRING_MAX_LEN 128U
#endif

/**
 * @brief  最大保存的未知帧原始数据长度。
 */
#ifndef TJC_PROTOCOL_UNKNOWN_MAX_LEN
#define TJC_PROTOCOL_UNKNOWN_MAX_LEN 32U
#endif

/**
 * @brief  当接收数据长期无结束符时的丢弃阈值。
 */
#ifndef TJC_PROTOCOL_STALL_THRESHOLD
#define TJC_PROTOCOL_STALL_THRESHOLD 128U
#endif

/**
 * @brief  读取流中指定偏移字节的函数类型。
 */
typedef uint8_t (*TJC_ProtocolPeekFn_t)(uint16_t position);

/**
 * @brief  协议消息类型。
 */
typedef enum {
  TJC_PROTOCOL_MSG_EXECUTE_RESULT = 0,
  TJC_PROTOCOL_MSG_BUFFER_OVERFLOW,
  TJC_PROTOCOL_MSG_TOUCH_EVENT,
  TJC_PROTOCOL_MSG_PAGE_ID,
  TJC_PROTOCOL_MSG_TOUCH_COORD,
  TJC_PROTOCOL_MSG_SLEEP_TOUCH,
  TJC_PROTOCOL_MSG_STRING_DATA,
  TJC_PROTOCOL_MSG_NUMERIC_DATA,
  TJC_PROTOCOL_MSG_AUTO_SLEEP,
  TJC_PROTOCOL_MSG_AUTO_WAKE,
  TJC_PROTOCOL_MSG_STARTUP,
  TJC_PROTOCOL_MSG_SD_UPGRADE,
  TJC_PROTOCOL_MSG_TRANSPARENT_FINISHED,
  TJC_PROTOCOL_MSG_TRANSPARENT_READY,
  TJC_PROTOCOL_MSG_UNKNOWN
} TJC_ProtocolMessageType_t;

/**
 * @brief  协议解析结果。
 */
typedef enum {
  TJC_PROTOCOL_PARSE_OK = 0,
  TJC_PROTOCOL_PARSE_NEED_MORE_DATA,
  TJC_PROTOCOL_PARSE_DROP_BYTES,
  TJC_PROTOCOL_PARSE_INVALID_PARAM
} TJC_ProtocolParseResult_t;

/**
 * @brief  触摸事件状态。
 */
typedef enum {
  TJC_TOUCH_RELEASE = 0x00,
  TJC_TOUCH_PRESS = 0x01
} TJC_TouchEvent_t;

/**
 * @brief  协议消息结构体。
 */
typedef struct {
  TJC_ProtocolMessageType_t type;
  uint8_t code;
  uint16_t frameLength;
  union {
    struct {
      uint8_t resultCode;
      bool isError;
    } executeResult;
    struct {
      uint8_t pageId;
      uint8_t componentId;
      uint8_t touchEvent;
    } touchEvent;
    struct {
      uint8_t pageId;
    } pageInfo;
    struct {
      uint16_t x;
      uint16_t y;
      uint8_t touchEvent;
    } touchCoord;
    struct {
      char text[TJC_PROTOCOL_STRING_MAX_LEN + 1U];
      uint16_t textLength;
      bool truncated;
    } stringData;
    struct {
      uint32_t value;
    } numericData;
    struct {
      uint8_t bytes[TJC_PROTOCOL_UNKNOWN_MAX_LEN];
      uint16_t length;
      bool truncated;
    } unknownData;
  } payload;
} TJC_ProtocolMessage_t;

/**
 * @brief  尝试从接收流中解析一帧协议数据。
 * @param  available 当前可读字节数。
 * @param  peek 读取指定偏移字节的回调函数。
 * @param  message 解析成功后的输出消息。
 * @param  consumedBytes 本次应消费的字节数。
 * @retval 协议解析结果。
 */
TJC_ProtocolParseResult_t TJC_ProtocolTryParse(uint16_t available,
                                               TJC_ProtocolPeekFn_t peek,
                                               TJC_ProtocolMessage_t *message,
                                               uint16_t *consumedBytes);

/**
 * @brief  判断执行结果码是否表示错误。
 * @param  resultCode 串口屏返回结果码。
 * @retval true 表示错误，false 表示非错误。
 */
bool TJC_ProtocolIsErrorResult(uint8_t resultCode);

/**
 * @brief  获取执行结果码对应的中文说明。
 * @param  resultCode 串口屏返回结果码。
 * @retval 中文说明字符串。
 */
const char *TJC_ProtocolResultCodeString(uint8_t resultCode);

/**
 * @brief  获取消息类型对应的中文名称。
 * @param  type 消息类型。
 * @retval 中文名称字符串。
 */
const char *TJC_ProtocolMessageTypeString(TJC_ProtocolMessageType_t type);

#endif
