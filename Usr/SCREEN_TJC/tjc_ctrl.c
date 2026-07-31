#include "tjc_ctrl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief  校验曲线控件目标与通道号是否合法。
 * @param  target 曲线控件名称或跨页面全路径。
 * @param  channel 通道号。
 * @retval true 参数合法，false 参数非法。
 */
static bool TJC_CtrlValidateCurveArgs(const char *target, uint8_t channel)
{
  if ((target == NULL) || (channel >= TJC_CURVE_MAX_CHANNELS)) {
    return false;
  }

  return true;
}

/**
 * @brief  计算 10 的指定次幂，用于虚拟浮点数缩放。
 * @param  decimalDigits 小数位数。
 * @param  scale 输出缩放值。
 * @retval true 计算成功，false 参数非法。
 */
static bool TJC_CtrlGetFloatScale(uint8_t decimalDigits, float *scale)
{
  uint8_t index;

  if ((scale == NULL) || (decimalDigits > TJC_VIRTUAL_FLOAT_MAX_DECIMALS)) {
    return false;
  }

  *scale = 1.0f;
  for (index = 0U; index < decimalDigits; index++) {
    *scale *= 10.0f;
  }

  return true;
}

/**
 * @brief  清空当前缓存的最近一条协议消息，避免旧数据干扰同步读取。
 */
static void TJC_CtrlFlushLastMessage(void)
{
  TJC_ProtocolMessage_t message;

  TJC_ParseIncomingData();
  while (TJC_GetLastMessage(&message)) {
  }
}

/** Discard only already-published messages without re-entering the parser. */
static void TJC_CtrlDiscardPublishedMessages(void)
{
  TJC_ProtocolMessage_t message;

  while (TJC_GetLastMessage(&message)) {
  }
}

/**
 * @brief  等待并获取 get 指令返回的数值型消息。
 * @param  numericValue 输出的原始数值。
 * @param  timeoutMs 超时时间。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_CtrlWaitNumericValue(uint32_t *numericValue, uint32_t timeoutMs)
{
  TJC_ProtocolMessage_t message;
  uint32_t startTick;
  uint32_t effectiveTimeout;

  if (numericValue == NULL) {
    return HAL_ERROR;
  }

  effectiveTimeout = (timeoutMs == 0U) ? TJC_CTRL_GET_TIMEOUT_MS : timeoutMs;
  startTick = HAL_GetTick();

  while ((HAL_GetTick() - startTick) < effectiveTimeout) {
    TJC_ParseIncomingData();

    if (!TJC_GetLastMessage(&message)) {
      continue;
    }

    if (message.type == TJC_PROTOCOL_MSG_NUMERIC_DATA) {
      *numericValue = message.payload.numericData.value;
      return HAL_OK;
    }
  }

  return HAL_TIMEOUT;
}

/**
 * @brief  等待并获取透传就绪消息。
 * @param  timeoutMs 超时时间。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_CtrlWaitTransparentReady(uint32_t timeoutMs)
{
  TJC_ProtocolMessage_t message;
  uint32_t startTick;
  uint32_t effectiveTimeout;

  effectiveTimeout = (timeoutMs == 0U) ? TJC_CTRL_TRANSPARENT_READY_TIMEOUT_MS : timeoutMs;
  startTick = HAL_GetTick();

  while ((HAL_GetTick() - startTick) < effectiveTimeout) {
    TJC_ParseIncomingData();

    if (!TJC_GetLastMessage(&message)) {
      continue;
    }

    if (message.type == TJC_PROTOCOL_MSG_TRANSPARENT_READY) {
      return HAL_OK;
    }

    if ((message.type == TJC_PROTOCOL_MSG_EXECUTE_RESULT) && message.payload.executeResult.isError) {
      return HAL_ERROR;
    }
  }

  return HAL_TIMEOUT;
}

/**
 * @brief  发送格式化后的控件命令。
 * @param  command 完整命令字符串。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_CtrlSendFormatted(const char *command)
{
  if (command == NULL) {
    return HAL_ERROR;
  }

  return TJC_SendString(command);
}

/**
 * @brief  对文本内容做串口屏语法所需的转义处理。
 * @param  source 原始文本。
 * @param  output 输出缓存。
 * @param  outputSize 输出缓存长度。
 * @retval true 转义成功，false 缓冲区不足或参数无效。
 */
static bool TJC_CtrlEscapeText(const char *source, char *output, uint16_t outputSize)
{
  uint16_t srcIndex;
  uint16_t dstIndex;

  if ((source == NULL) || (output == NULL) || (outputSize == 0U)) {
    return false;
  }

  srcIndex = 0U;
  dstIndex = 0U;

  while (source[srcIndex] != '\0') {
    char ch = source[srcIndex];

    if ((ch == '\\') || (ch == '"')) {
      if ((uint16_t)(dstIndex + 2U) >= outputSize) {
        return false;
      }
      output[dstIndex++] = '\\';
      output[dstIndex++] = ch;
    } else if (ch == '\r') {
      if ((uint16_t)(dstIndex + 2U) >= outputSize) {
        return false;
      }
      output[dstIndex++] = '\\';
      output[dstIndex++] = 'r';
    } else if (ch == '\n') {
      if ((uint16_t)(dstIndex + 2U) >= outputSize) {
        return false;
      }
      output[dstIndex++] = '\\';
      output[dstIndex++] = 'n';
    } else {
      if ((uint16_t)(dstIndex + 1U) >= outputSize) {
        return false;
      }
      output[dstIndex++] = ch;
    }

    srcIndex++;
  }

  output[dstIndex] = '\0';
  return true;
}

/**
 * @brief  使用 snprintf 生成命令并发送。
 * @param  format 格式化模板。
 * @param  arg1 字符串参数1。
 * @param  arg2 字符串参数2。
 * @param  value 数值参数。
 * @retval HAL 状态值。
 */
static HAL_StatusTypeDef TJC_CtrlSendValueCommand(const char *format,
                                                  const char *arg1,
                                                  const char *arg2,
                                                  int32_t value)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), format, arg1, arg2, (long)value);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  发送一条完整的原始控件/系统指令。
 * @param  command 完整指令字符串，不需要附加帧尾。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSendCommand(const char *command)
{
  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  Request a supported TJC display to sound its buzzer.
 * @param  durationMs Buzzer duration in milliseconds, 1-65535.
 * @retval HAL status value.
 */
HAL_StatusTypeDef TJC_SystemBeep(uint16_t durationMs)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int written;

  if (durationMs == 0U) {
    return HAL_ERROR;
  }
  written = snprintf(
      command, sizeof(command), "beep %u",
      (unsigned int)durationMs);
  if ((written < 0) || ((size_t)written >= sizeof(command))) {
    return HAL_ERROR;
  }
  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  向目标属性写入数值。
 * @param  target 控件名称或跨页面全路径。
 * @param  attribute 属性名。
 * @param  value 数值内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSetValue(const char *target, const char *attribute, int32_t value)
{
  if ((target == NULL) || (attribute == NULL)) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendValueCommand("%s.%s=%ld", target, attribute, value);
}

/**
 * @brief  向目标属性写入文本。
 * @param  target 控件名称或跨页面全路径。
 * @param  attribute 属性名。
 * @param  text 文本内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSetText(const char *target, const char *attribute, const char *text)
{
  char escaped[TJC_CTRL_TEXT_MAX_LEN * 2U + 1U];
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if ((target == NULL) || (attribute == NULL) || (text == NULL)) {
    return HAL_ERROR;
  }

  if (!TJC_CtrlEscapeText(text, escaped, (uint16_t)sizeof(escaped))) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "%s.%s=\"%s\"", target, attribute, escaped);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  向目标属性写入原始表达式。
 * @param  target 控件名称或跨页面全路径。
 * @param  attribute 属性名。
 * @param  rawExpression 原始表达式内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSetRaw(const char *target, const char *attribute, const char *rawExpression)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if ((target == NULL) || (attribute == NULL) || (rawExpression == NULL)) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "%s.%s=%s", target, attribute, rawExpression);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  页面跳转到指定页面名称。
 * @param  pageName 页面名称。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_PageSet(const char *pageName)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (pageName == NULL) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "page %s", pageName);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  页面跳转到指定页面 ID。
 * @param  pageId 页面 ID。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_PageSetById(uint16_t pageId)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), "page %u", pageId);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  通过 vis 指令显示或隐藏控件。
 * @param  target 控件名称或跨页面全路径。
 * @param  visible true 显示，false 隐藏。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentSetVisible(const char *target, bool visible)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (target == NULL) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "vis %s,%u", target, visible ? 1U : 0U);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  通过 tsw 指令启用或禁用控件触摸。
 * @param  target 控件名称或跨页面全路径。
 * @param  enabled true 启用，false 禁用。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentSetTouch(const char *target, bool enabled)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (target == NULL) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "tsw %s,%u", target, enabled ? 1U : 0U);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  通过 click 指令触发控件按下或弹起事件。
 * @param  target 控件名称或跨页面全路径。
 * @param  pressed true 按下事件，false 弹起事件。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentClick(const char *target, bool pressed)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (target == NULL) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "click %s,%u", target, pressed ? 1U : 0U);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  通过 prints 指令请求屏幕从串口输出变量或文本。
 * @param  expression 变量表达式。
 * @param  format 输出格式。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentPrint(const char *expression, uint8_t format)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (expression == NULL) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "prints %s,%u", expression, format);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  请求获取指定表达式的值，返回结果由协议解析层接收。
 * @param  expression 变量表达式。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentGet(const char *expression)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (expression == NULL) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "get %s", expression);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  同步读取数值型表达式，并回填到有符号整型变量。
 * @param  expression 变量表达式。
 * @param  value 输出变量地址。
 * @param  timeoutMs 超时时间。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentGetInt32(const char *expression, int32_t *value, uint32_t timeoutMs)
{
  HAL_StatusTypeDef status;
  uint32_t rawValue;

  if ((expression == NULL) || (value == NULL)) {
    return HAL_ERROR;
  }

  TJC_CtrlFlushLastMessage();

  status = TJC_ComponentGet(expression);
  if (status != HAL_OK) {
    return status;
  }

  status = TJC_CtrlWaitNumericValue(&rawValue, timeoutMs);
  if (status != HAL_OK) {
    return status;
  }

  *value = (int32_t)rawValue;
  return HAL_OK;
}

/**
 * @brief  同步读取数值型表达式，并回填到无符号整型变量。
 * @param  expression 变量表达式。
 * @param  value 输出变量地址。
 * @param  timeoutMs 超时时间。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentGetUInt32(const char *expression, uint32_t *value, uint32_t timeoutMs)
{
  HAL_StatusTypeDef status;
  int32_t signedValue;

  if ((expression == NULL) || (value == NULL)) {
    return HAL_ERROR;
  }

  status = TJC_ComponentGetInt32(expression, &signedValue, timeoutMs);
  if (status != HAL_OK) {
    return status;
  }

  if (signedValue < 0) {
    return HAL_ERROR;
  }

  *value = (uint32_t)signedValue;
  return HAL_OK;
}

/**
 * @brief  按虚拟浮点数规则同步读取数值，并回填到浮点变量。
 * @param  expression 变量表达式。
 * @param  decimalDigits 小数位数。
 * @param  value 输出变量地址。
 * @param  timeoutMs 超时时间。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentGetFloat(const char *expression,
                                        uint8_t decimalDigits,
                                        float *value,
                                        uint32_t timeoutMs)
{
  HAL_StatusTypeDef status;
  int32_t rawValue;
  float scale;

  if ((expression == NULL) || (value == NULL)) {
    return HAL_ERROR;
  }

  if (!TJC_CtrlGetFloatScale(decimalDigits, &scale)) {
    return HAL_ERROR;
  }

  status = TJC_ComponentGetInt32(expression, &rawValue, timeoutMs);
  if (status != HAL_OK) {
    return status;
  }

  *value = ((float)rawValue) / scale;
  return HAL_OK;
}

/**
 * @brief  向曲线控件指定通道追加一个数据点。
 * @param  target 曲线控件名称或跨页面全路径。
 * @param  channel 通道号。
 * @param  value 数据值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveAddValue(const char *target, uint8_t channel, uint8_t value)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (!TJC_CtrlValidateCurveArgs(target, channel)) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "add %s.id,%u,%u", target, channel, value);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  发送曲线透传准备指令 addt。
 * @param  target 曲线控件名称（addt 不支持跨页面）。
 * @param  channel 通道号。
 * @param  len 透传数据点数量，范围 1-1024。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurvePreparePassThrough(const char *target, uint8_t channel, uint16_t len)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if ((!TJC_CtrlValidateCurveArgs(target, channel)) || (len == 0U)) {
    return HAL_ERROR;
  }

  if (strchr(target, '.') != NULL) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "addt %s.id,%u,%u", target, channel, len);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  /* The application parser may run from TIM7; do not parse re-entrantly here. */
  TJC_CtrlDiscardPublishedMessages();

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  等待串口屏返回透传就绪消息（0xFE）。
 * @param  timeoutMs 超时时间。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveWaitPassThroughReady(uint32_t timeoutMs)
{
  return TJC_CtrlWaitTransparentReady(timeoutMs);
}

/**
 * @brief  发送曲线透传数据并结束透传。
 * @param  data 原始数据数组。
 * @param  len 透传数据点数量，范围 1-1024。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveSendPassThroughData(const uint8_t *data, uint16_t len)
{
  HAL_StatusTypeDef status;
  uint8_t passThroughEnd;

  if ((data == NULL) || (len == 0U)) {
    return HAL_ERROR;
  }

  status = TJC_SendBytes(data, len, false);
  if (status != HAL_OK) {
    return status;
  }

  passThroughEnd = 0x01U;
  return TJC_SendBytes(&passThroughEnd, 1U, true);
}

/**
 * @brief  向曲线控件指定通道透传一段原始数据（封装完整两阶段流程）。
 * @param  target 曲线控件名称（addt 不支持跨页面）。
 * @param  channel 通道号。
 * @param  data 原始数据数组。
 * @param  len 透传数据点数量，范围 1-1024。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveStartPassThrough(const char *target,
                                            uint8_t channel,
                                            const uint8_t *data,
                                            uint16_t len)
{
  HAL_StatusTypeDef status;

  status = TJC_CurvePreparePassThrough(target, channel, len);
  if (status != HAL_OK) {
    return status;
  }

  status = TJC_CurveWaitPassThroughReady(0U);
  if (status != HAL_OK) {
    return status;
  }

  return TJC_CurveSendPassThroughData(data, len);
}

/**
 * @brief  清除曲线控件指定通道中的数据。
 * @param  target 曲线控件名称或跨页面全路径。
 * @param  channel 通道号。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveClearChannel(const char *target, uint8_t channel)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  if (!TJC_CtrlValidateCurveArgs(target, channel)) {
    return HAL_ERROR;
  }

  length = snprintf(command, sizeof(command), "cle %s.id,%u", target, channel);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  设置文本控件显示内容。
 * @param  target 文本控件名称或跨页面全路径。
 * @param  text 文本内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_TextSetText(const char *target, const char *text)
{
  return TJC_CtrlSetText(target, "txt", text);
}

/**
 * @brief  设置文本控件字体 ID。
 * @param  target 文本控件名称或跨页面全路径。
 * @param  fontId 字体 ID。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_TextSetFont(const char *target, uint16_t fontId)
{
  return TJC_CtrlSetValue(target, "font", fontId);
}

/**
 * @brief  设置数字/变量/进度类控件数值。
 * @param  target 控件名称或跨页面全路径。
 * @param  value 数值内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ValueSet(const char *target, int32_t value)
{
  return TJC_CtrlSetValue(target, "val", value);
}

/**
 * @brief  设置按钮控件文本。
 * @param  target 按钮控件名称或跨页面全路径。
 * @param  text 文本内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ButtonSetText(const char *target, const char *text)
{
  return TJC_CtrlSetText(target, "txt", text);
}

/**
 * @brief  设置双态按钮、复选框、单选框选中状态。
 * @param  target 控件名称或跨页面全路径。
 * @param  checked true 选中，false 取消。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_BinaryStateSet(const char *target, bool checked)
{
  return TJC_CtrlSetValue(target, "val", checked ? 1 : 0);
}

/**
 * @brief  设置进度条或滑块的当前数值。
 * @param  target 控件名称或跨页面全路径。
 * @param  value 数值内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ProgressSetValue(const char *target, int32_t value)
{
  return TJC_CtrlSetValue(target, "val", value);
}

/**
 * @brief  设置图片或页面背景图片资源 ID。
 * @param  target 控件名称或跨页面全路径。
 * @param  pictureId 图片资源 ID。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_PictureSetId(const char *target, uint16_t pictureId)
{
  return TJC_CtrlSetValue(target, "pic", pictureId);
}

/**
 * @brief  设置系统变量 dim 当前亮度。
 * @param  brightness 亮度值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBrightness(uint16_t brightness)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), "dim=%u", brightness);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  设置系统变量 dims 上电默认亮度。
 * @param  brightness 亮度值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBootBrightness(uint16_t brightness)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), "dims=%u", brightness);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  设置当前波特率变量 baud。
 * @param  baudrate 波特率值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBaud(uint32_t baudrate)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), "baud=%lu", (unsigned long)baudrate);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  设置上电默认波特率变量 bauds。
 * @param  baudrate 波特率值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBootBaud(uint32_t baudrate)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), "bauds=%lu", (unsigned long)baudrate);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  设置串口返回状态变量 bkcmd。
 * @param  mode 模式值，推荐 0-3。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBkcmd(uint8_t mode)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), "bkcmd=%u", mode);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}

/**
 * @brief  发送 sleep=1 使屏幕进入睡眠。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSleep(void)
{
  return TJC_CtrlSendFormatted("sleep=1");
}

/**
 * @brief  发送 sleep=0 使屏幕退出睡眠。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemWakeup(void)
{
  return TJC_CtrlSendFormatted("sleep=0");
}

/**
 * @brief  发送 randset 指令设置随机数范围。
 * @param  minValue 最小值。
 * @param  maxValue 最大值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetRandomRange(int32_t minValue, int32_t maxValue)
{
  char command[TJC_CTRL_CMD_MAX_LEN];
  int length;

  length = snprintf(command, sizeof(command), "randset %ld,%ld", (long)minValue, (long)maxValue);
  if ((length <= 0) || (length >= (int)sizeof(command))) {
    return HAL_ERROR;
  }

  return TJC_CtrlSendFormatted(command);
}
