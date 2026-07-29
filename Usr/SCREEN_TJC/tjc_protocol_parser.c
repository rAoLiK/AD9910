#include "tjc_protocol_parser.h"

#include <string.h>

/**
 * @brief  判断指定偏移位置是否为协议结束符。
 * @param  peek 读取字节回调函数。
 * @param  start 起始偏移。
 * @retval true 是结束符，false 不是结束符。
 */
static bool TJC_IsTerminatorAt(TJC_ProtocolPeekFn_t peek, uint16_t start)
{
  return (peek(start) == TJC_PROTOCOL_END_BYTE)
    && (peek((uint16_t)(start + 1U)) == TJC_PROTOCOL_END_BYTE)
    && (peek((uint16_t)(start + 2U)) == TJC_PROTOCOL_END_BYTE);
}

/**
 * @brief  搜索协议结束符位置。
 * @param  available 当前可读字节数。
 * @param  peek 读取字节回调函数。
 * @param  terminatorStart 结束符起始偏移输出。
 * @retval true 找到结束符，false 未找到结束符。
 */
static bool TJC_FindTerminator(uint16_t available,
                               TJC_ProtocolPeekFn_t peek,
                               uint16_t *terminatorStart)
{
  uint16_t index;

  if (available < TJC_PROTOCOL_END_LEN) {
    return false;
  }

  for (index = 0U; index <= (uint16_t)(available - TJC_PROTOCOL_END_LEN); index++) {
    if (TJC_IsTerminatorAt(peek, index)) {
      *terminatorStart = index;
      return true;
    }
  }

  return false;
}

/**
 * @brief  获取固定长度消息的总帧长。
 * @param  code 协议首字节。
 * @retval 总帧长，0 表示变长或未知。
 */
static uint16_t TJC_FixedFrameLength(uint8_t code)
{
  switch (code) {
    case 0x00U:
    case 0x01U:
    case 0x02U:
    case 0x03U:
    case 0x04U:
    case 0x05U:
    case 0x06U:
    case 0x09U:
    case 0x11U:
    case 0x12U:
    case 0x1AU:
    case 0x1BU:
    case 0x1CU:
    case 0x1DU:
    case 0x1EU:
    case 0x1FU:
    case 0x20U:
    case 0x23U:
    case 0x24U:
    case 0x86U:
    case 0x87U:
    case 0x88U:
    case 0x89U:
    case 0xFDU:
    case 0xFEU:
      return 4U;

    case 0x66U:
      return 5U;

    case 0x65U:
      return 7U;

    case 0x71U:
      return 8U;

    case 0x67U:
    case 0x68U:
      return 9U;

    default:
      return 0U;
  }
}

/**
 * @brief  判断首字节是否为已知协议类型。
 * @param  code 协议首字节。
 * @retval true 已知，false 未知。
 */
static bool TJC_IsKnownCode(uint8_t code)
{
  return (TJC_FixedFrameLength(code) != 0U) || (code == 0x70U);
}

/**
 * @brief  解析字符串返回帧。
 * @param  available 当前可读字节数。
 * @param  peek 读取字节回调函数。
 * @param  message 输出消息。
 * @param  consumedBytes 消费字节数输出。
 * @retval 协议解析结果。
 */
static TJC_ProtocolParseResult_t TJC_ParseStringFrame(uint16_t available,
                                                      TJC_ProtocolPeekFn_t peek,
                                                      TJC_ProtocolMessage_t *message,
                                                      uint16_t *consumedBytes)
{
  uint16_t terminatorStart;
  uint16_t stringLength;
  uint16_t index;

  if (!TJC_FindTerminator(available, peek, &terminatorStart)) {
    if (available >= TJC_PROTOCOL_STALL_THRESHOLD) {
      *consumedBytes = 1U;
      return TJC_PROTOCOL_PARSE_DROP_BYTES;
    }
    return TJC_PROTOCOL_PARSE_NEED_MORE_DATA;
  }

  if (terminatorStart == 0U) {
    *consumedBytes = 1U;
    return TJC_PROTOCOL_PARSE_DROP_BYTES;
  }

  message->type = TJC_PROTOCOL_MSG_STRING_DATA;
  message->code = 0x70U;
  message->frameLength = (uint16_t)(terminatorStart + TJC_PROTOCOL_END_LEN);

  stringLength = (uint16_t)(terminatorStart - 1U);
  message->payload.stringData.truncated = (stringLength > TJC_PROTOCOL_STRING_MAX_LEN);
  message->payload.stringData.textLength = (stringLength > TJC_PROTOCOL_STRING_MAX_LEN)
    ? TJC_PROTOCOL_STRING_MAX_LEN
    : stringLength;

  for (index = 0U; index < message->payload.stringData.textLength; index++) {
    message->payload.stringData.text[index] = (char)peek((uint16_t)(index + 1U));
  }
  message->payload.stringData.text[message->payload.stringData.textLength] = '\0';

  *consumedBytes = message->frameLength;
  return TJC_PROTOCOL_PARSE_OK;
}

/**
 * @brief  解析已知固定长度帧。
 * @param  code 协议首字节。
 * @param  frameLength 固定总帧长。
 * @param  peek 读取字节回调函数。
 * @param  message 输出消息。
 */
static void TJC_ParseFixedFrame(uint8_t code,
                                uint16_t frameLength,
                                TJC_ProtocolPeekFn_t peek,
                                TJC_ProtocolMessage_t *message)
{
  uint32_t value;

  message->code = code;
  message->frameLength = frameLength;

  switch (code) {
    case 0x00U:
    case 0x01U:
    case 0x02U:
    case 0x03U:
    case 0x04U:
    case 0x05U:
    case 0x06U:
    case 0x09U:
    case 0x11U:
    case 0x12U:
    case 0x1AU:
    case 0x1BU:
    case 0x1CU:
    case 0x1DU:
    case 0x1EU:
    case 0x1FU:
    case 0x20U:
    case 0x23U:
      message->type = TJC_PROTOCOL_MSG_EXECUTE_RESULT;
      message->payload.executeResult.resultCode = code;
      message->payload.executeResult.isError = TJC_ProtocolIsErrorResult(code);
      break;

    case 0x24U:
      message->type = TJC_PROTOCOL_MSG_BUFFER_OVERFLOW;
      break;

    case 0x65U:
      message->type = TJC_PROTOCOL_MSG_TOUCH_EVENT;
      message->payload.touchEvent.pageId = peek(1U);
      message->payload.touchEvent.componentId = peek(2U);
      message->payload.touchEvent.touchEvent = peek(3U);
      break;

    case 0x66U:
      message->type = TJC_PROTOCOL_MSG_PAGE_ID;
      message->payload.pageInfo.pageId = peek(1U);
      break;

    case 0x67U:
    case 0x68U:
      message->type = (code == 0x67U) ? TJC_PROTOCOL_MSG_TOUCH_COORD : TJC_PROTOCOL_MSG_SLEEP_TOUCH;
      message->payload.touchCoord.x = (uint16_t)(((uint16_t)peek(1U) << 8) | peek(2U));
      message->payload.touchCoord.y = (uint16_t)(((uint16_t)peek(3U) << 8) | peek(4U));
      message->payload.touchCoord.touchEvent = peek(5U);
      break;

    case 0x71U:
      message->type = TJC_PROTOCOL_MSG_NUMERIC_DATA;
      value = ((uint32_t)peek(1U))
        | ((uint32_t)peek(2U) << 8)
        | ((uint32_t)peek(3U) << 16)
        | ((uint32_t)peek(4U) << 24);
      message->payload.numericData.value = value;
      break;

    case 0x86U:
      message->type = TJC_PROTOCOL_MSG_AUTO_SLEEP;
      break;

    case 0x87U:
      message->type = TJC_PROTOCOL_MSG_AUTO_WAKE;
      break;

    case 0x88U:
      message->type = TJC_PROTOCOL_MSG_STARTUP;
      break;

    case 0x89U:
      message->type = TJC_PROTOCOL_MSG_SD_UPGRADE;
      break;

    case 0xFDU:
      message->type = TJC_PROTOCOL_MSG_TRANSPARENT_FINISHED;
      break;

    case 0xFEU:
      message->type = TJC_PROTOCOL_MSG_TRANSPARENT_READY;
      break;

    default:
      message->type = TJC_PROTOCOL_MSG_UNKNOWN;
      message->payload.unknownData.length = 1U;
      message->payload.unknownData.truncated = false;
      message->payload.unknownData.bytes[0] = code;
      break;
  }
}

/**
 * @brief  解析未知帧并保留原始数据。
 * @param  available 当前可读字节数。
 * @param  peek 读取字节回调函数。
 * @param  message 输出消息。
 * @param  consumedBytes 消费字节数输出。
 * @retval 协议解析结果。
 */
static TJC_ProtocolParseResult_t TJC_ParseUnknownFrame(uint16_t available,
                                                       TJC_ProtocolPeekFn_t peek,
                                                       TJC_ProtocolMessage_t *message,
                                                       uint16_t *consumedBytes)
{
  uint16_t terminatorStart;
  uint16_t rawLength;
  uint16_t index;

  if (!TJC_FindTerminator(available, peek, &terminatorStart)) {
    if (available >= TJC_PROTOCOL_STALL_THRESHOLD) {
      *consumedBytes = 1U;
      return TJC_PROTOCOL_PARSE_DROP_BYTES;
    }
    return TJC_PROTOCOL_PARSE_NEED_MORE_DATA;
  }

  message->type = TJC_PROTOCOL_MSG_UNKNOWN;
  message->code = peek(0U);
  message->frameLength = (uint16_t)(terminatorStart + TJC_PROTOCOL_END_LEN);

  rawLength = message->frameLength;
  message->payload.unknownData.truncated = (rawLength > TJC_PROTOCOL_UNKNOWN_MAX_LEN);
  message->payload.unknownData.length = (rawLength > TJC_PROTOCOL_UNKNOWN_MAX_LEN)
    ? TJC_PROTOCOL_UNKNOWN_MAX_LEN
    : rawLength;

  for (index = 0U; index < message->payload.unknownData.length; index++) {
    message->payload.unknownData.bytes[index] = peek(index);
  }

  *consumedBytes = message->frameLength;
  return TJC_PROTOCOL_PARSE_OK;
}

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
                                               uint16_t *consumedBytes)
{
  uint8_t code;
  uint16_t frameLength;

  if ((peek == NULL) || (message == NULL) || (consumedBytes == NULL)) {
    return TJC_PROTOCOL_PARSE_INVALID_PARAM;
  }

  *consumedBytes = 0U;
  if (available < 4U) {
    return TJC_PROTOCOL_PARSE_NEED_MORE_DATA;
  }

  memset(message, 0, sizeof(*message));
  code = peek(0U);

  if (code == 0x70U) {
    return TJC_ParseStringFrame(available, peek, message, consumedBytes);
  }

  frameLength = TJC_FixedFrameLength(code);
  if (frameLength != 0U) {
    if (available < frameLength) {
      return TJC_PROTOCOL_PARSE_NEED_MORE_DATA;
    }

    if (!TJC_IsTerminatorAt(peek, (uint16_t)(frameLength - TJC_PROTOCOL_END_LEN))) {
      *consumedBytes = 1U;
      return TJC_PROTOCOL_PARSE_DROP_BYTES;
    }

    TJC_ParseFixedFrame(code, frameLength, peek, message);
    *consumedBytes = frameLength;
    return TJC_PROTOCOL_PARSE_OK;
  }

  if (!TJC_IsKnownCode(code)) {
    return TJC_ParseUnknownFrame(available, peek, message, consumedBytes);
  }

  *consumedBytes = 1U;
  return TJC_PROTOCOL_PARSE_DROP_BYTES;
}

/**
 * @brief  判断执行结果码是否表示错误。
 * @param  resultCode 串口屏返回结果码。
 * @retval true 表示错误，false 表示非错误。
 */
bool TJC_ProtocolIsErrorResult(uint8_t resultCode)
{
  return resultCode != 0x01U;
}

/**
 * @brief  获取执行结果码对应的中文说明。
 * @param  resultCode 串口屏返回结果码。
 * @retval 中文说明字符串。
 */
const char *TJC_ProtocolResultCodeString(uint8_t resultCode)
{
  switch (resultCode) {
    case 0x00U: return "无效指令";
    case 0x01U: return "指令成功执行";
    case 0x02U: return "控件ID无效";
    case 0x03U: return "页面ID无效";
    case 0x04U: return "图片ID无效";
    case 0x05U: return "字库ID无效";
    case 0x06U: return "文件操作失败";
    case 0x09U: return "CRC校验失败";
    case 0x11U: return "波特率设置无效";
    case 0x12U: return "曲线控件ID号或通道号无效";
    case 0x1AU: return "变量名称无效";
    case 0x1BU: return "变量运算无效";
    case 0x1CU: return "赋值操作失败";
    case 0x1DU: return "掉电存储空间操作失败";
    case 0x1EU: return "参数数量无效";
    case 0x1FU: return "IO操作失败";
    case 0x20U: return "转义字符使用错误";
    case 0x23U: return "变量名称太长";
    case 0x24U: return "串口缓冲区溢出";
    default: return "未知执行结果码";
  }
}

/**
 * @brief  获取消息类型对应的中文名称。
 * @param  type 消息类型。
 * @retval 中文名称字符串。
 */
const char *TJC_ProtocolMessageTypeString(TJC_ProtocolMessageType_t type)
{
  switch (type) {
    case TJC_PROTOCOL_MSG_EXECUTE_RESULT: return "执行结果";
    case TJC_PROTOCOL_MSG_BUFFER_OVERFLOW: return "屏幕串口缓冲区溢出";
    case TJC_PROTOCOL_MSG_TOUCH_EVENT: return "控件触摸事件";
    case TJC_PROTOCOL_MSG_PAGE_ID: return "当前页面ID";
    case TJC_PROTOCOL_MSG_TOUCH_COORD: return "触摸坐标";
    case TJC_PROTOCOL_MSG_SLEEP_TOUCH: return "睡眠触摸事件";
    case TJC_PROTOCOL_MSG_STRING_DATA: return "字符串返回值";
    case TJC_PROTOCOL_MSG_NUMERIC_DATA: return "数值返回值";
    case TJC_PROTOCOL_MSG_AUTO_SLEEP: return "自动进入睡眠";
    case TJC_PROTOCOL_MSG_AUTO_WAKE: return "自动唤醒";
    case TJC_PROTOCOL_MSG_STARTUP: return "系统启动成功";
    case TJC_PROTOCOL_MSG_SD_UPGRADE: return "开始SD卡升级";
    case TJC_PROTOCOL_MSG_TRANSPARENT_FINISHED: return "透传完成";
    case TJC_PROTOCOL_MSG_TRANSPARENT_READY: return "透传就绪";
    case TJC_PROTOCOL_MSG_UNKNOWN: return "未知协议帧";
    default: return "未定义消息类型";
  }
}
