#ifndef __TJC_CTRL_H__
#define __TJC_CTRL_H__

#include "tjc_usart_hmi.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief  控件支持库内部命令缓存长度。
 */
#ifndef TJC_CTRL_CMD_MAX_LEN
#define TJC_CTRL_CMD_MAX_LEN 192U
#endif

/**
 * @brief  字符串转义后最大缓存长度。
 */
#ifndef TJC_CTRL_TEXT_MAX_LEN
#define TJC_CTRL_TEXT_MAX_LEN 128U
#endif

/**
 * @brief  同步等待 get 返回结果的默认超时时间。
 */
#ifndef TJC_CTRL_GET_TIMEOUT_MS
#define TJC_CTRL_GET_TIMEOUT_MS 200U
#endif

/**
 * @brief  等待曲线透传就绪消息的默认超时时间。
 */
#ifndef TJC_CTRL_TRANSPARENT_READY_TIMEOUT_MS
#define TJC_CTRL_TRANSPARENT_READY_TIMEOUT_MS 200U
#endif

/**
 * @brief  曲线控件最大通道数。
 */
#ifndef TJC_CURVE_MAX_CHANNELS
#define TJC_CURVE_MAX_CHANNELS 4U
#endif

/**
 * @brief  虚拟浮点数支持的最大小数位数。
 */
#ifndef TJC_VIRTUAL_FLOAT_MAX_DECIMALS
#define TJC_VIRTUAL_FLOAT_MAX_DECIMALS 8U
#endif

/**
 * @brief  prints 指令输出格式定义：十进制或原始字符串。
 */
#define TJC_PRINTS_FORMAT_DEC_OR_TEXT 0U

/**
 * @brief  prints 指令输出格式定义：1 字节十六进制。
 */
#define TJC_PRINTS_FORMAT_HEX_1BYTE 1U

/**
 * @brief  prints 指令输出格式定义：2 字节十六进制。
 */
#define TJC_PRINTS_FORMAT_HEX_2BYTE 2U

/**
 * @brief  prints 指令输出格式定义：4 字节十六进制。
 */
#define TJC_PRINTS_FORMAT_HEX_4BYTE 3U

/**
 * @brief  发送一条完整的原始控件/系统指令。
 * @param  command 完整指令字符串，不需要附加帧尾。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSendCommand(const char *command);

/**
 * @brief  向目标属性写入数值。
 * @param  target 控件名称或跨页面全路径，例如 t0、main.t0。
 * @param  attribute 属性名，例如 val、pic、font。
 * @param  value 数值内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSetValue(const char *target, const char *attribute, int32_t value);

/**
 * @brief  向目标属性写入文本。
 * @param  target 控件名称或跨页面全路径，例如 t0、main.t0。
 * @param  attribute 属性名，例如 txt。
 * @param  text 文本内容，内部会自动转义引号、反斜杠、回车换行。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSetText(const char *target, const char *attribute, const char *text);

/**
 * @brief  向目标属性写入原始表达式。
 * @param  target 控件名称或跨页面全路径。
 * @param  attribute 属性名。
 * @param  rawExpression 原始表达式内容，不自动添加引号。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CtrlSetRaw(const char *target, const char *attribute, const char *rawExpression);

/**
 * @brief  页面跳转到指定页面名称。
 * @param  pageName 页面名称。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_PageSet(const char *pageName);

/**
 * @brief  页面跳转到指定页面 ID。
 * @param  pageId 页面 ID。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_PageSetById(uint16_t pageId);

/**
 * @brief  通过 vis 指令显示或隐藏控件。
 * @param  target 控件名称或跨页面全路径。
 * @param  visible true 显示，false 隐藏。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentSetVisible(const char *target, bool visible);

/**
 * @brief  通过 tsw 指令启用或禁用控件触摸。
 * @param  target 控件名称或跨页面全路径。
 * @param  enabled true 启用，false 禁用。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentSetTouch(const char *target, bool enabled);

/**
 * @brief  通过 click 指令触发控件按下或弹起事件。
 * @param  target 控件名称或跨页面全路径。
 * @param  pressed true 按下事件，false 弹起事件。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentClick(const char *target, bool pressed);

/**
 * @brief  通过 prints 指令请求屏幕从串口输出变量或文本。
 * @param  expression 变量表达式，例如 n0.val、t0.txt。
 * @param  format 输出格式，使用 TJC_PRINTS_FORMAT_* 宏。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentPrint(const char *expression, uint8_t format);

/**
 * @brief  请求获取指定表达式的值，返回结果由协议解析层接收。
 * @param  expression 变量表达式，例如 n0.val、t0.txt。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentGet(const char *expression);

/**
 * @brief  同步读取数值型表达式，并回填到有符号整型变量。
 * @param  expression 变量表达式，例如 n0.val、va0.val。
 * @param  value 输出的有符号整型变量地址。
 * @param  timeoutMs 超时时间，传 0 使用默认值 TJC_CTRL_GET_TIMEOUT_MS。
 * @retval HAL 状态值。
 * @note   该接口会主动轮询解析返回消息，不建议在中断上下文中调用。
 */
HAL_StatusTypeDef TJC_ComponentGetInt32(const char *expression, int32_t *value, uint32_t timeoutMs);

/**
 * @brief  同步读取数值型表达式，并回填到无符号整型变量。
 * @param  expression 变量表达式，例如 n0.val、j0.val。
 * @param  value 输出的无符号整型变量地址。
 * @param  timeoutMs 超时时间，传 0 使用默认值 TJC_CTRL_GET_TIMEOUT_MS。
 * @retval HAL 状态值。
 * @note   若屏幕返回负数，该接口会返回 HAL_ERROR。
 */
HAL_StatusTypeDef TJC_ComponentGetUInt32(const char *expression, uint32_t *value, uint32_t timeoutMs);

/**
 * @brief  按虚拟浮点数规则同步读取数值，并回填到浮点变量。
 * @param  expression 变量表达式，例如 x0.val。
 * @param  decimalDigits 虚拟浮点数小数位数，对应控件属性 vvs1，范围 0-8。
 * @param  value 输出的浮点变量地址。
 * @param  timeoutMs 超时时间，传 0 使用默认值 TJC_CTRL_GET_TIMEOUT_MS。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ComponentGetFloat(const char *expression,
                                        uint8_t decimalDigits,
                                        float *value,
                                        uint32_t timeoutMs);

/**
 * @brief  向曲线控件指定通道追加一个数据点。
 * @param  target 曲线控件名称或跨页面全路径，例如 s0、main.s0。
 * @param  channel 通道号，范围 0-3。
 * @param  value 数据值，范围 0-255，且应不超过曲线控件高度。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveAddValue(const char *target, uint8_t channel, uint8_t value);

/**
 * @brief  发送曲线透传准备指令 addt。
 * @param  target 曲线控件名称（addt 不支持跨页面）。
 * @param  channel 通道号，范围 0-3。
 * @param  len 透传数据点数量，范围 1-1024。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurvePreparePassThrough(const char *target,
                                              uint8_t channel,
                                              uint16_t len);

/**
 * @brief  等待串口屏返回透传就绪消息（0xFE）。
 * @param  timeoutMs 超时时间，传 0 使用默认值 TJC_CTRL_TRANSPARENT_READY_TIMEOUT_MS。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveWaitPassThroughReady(uint32_t timeoutMs);

/**
 * @brief  发送曲线透传数据并结束透传。
 * @param  data 原始数据数组。
 * @param  len 透传数据点数量，范围 1-1024。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveSendPassThroughData(const uint8_t *data, uint16_t len);

/**
 * @brief  向曲线控件指定通道透传一段原始数据（封装完整两阶段流程）。
 * @param  target 曲线控件名称（addt 不支持跨页面）。
 * @param  channel 通道号，范围 0-3。
 * @param  data 原始数据数组。
 * @param  len 透传数据点数量，范围 1-1024。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveStartPassThrough(const char *target,
                                            uint8_t channel,
                                            const uint8_t *data,
                                            uint16_t len);

/**
 * @brief  清除曲线控件指定通道中的数据。
 * @param  target 曲线控件名称或跨页面全路径，例如 s0、main.s0。
 * @param  channel 通道号，范围 0-3。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_CurveClearChannel(const char *target, uint8_t channel);

/**
 * @brief  设置文本控件显示内容。
 * @param  target 文本控件名称或跨页面全路径。
 * @param  text 文本内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_TextSetText(const char *target, const char *text);

/**
 * @brief  设置文本控件字体 ID。
 * @param  target 文本控件名称或跨页面全路径。
 * @param  fontId 字体 ID。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_TextSetFont(const char *target, uint16_t fontId);

/**
 * @brief  设置数字/变量/进度类控件数值。
 * @param  target 控件名称或跨页面全路径。
 * @param  value 数值内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ValueSet(const char *target, int32_t value);

/**
 * @brief  设置按钮控件文本。
 * @param  target 按钮控件名称或跨页面全路径。
 * @param  text 文本内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ButtonSetText(const char *target, const char *text);

/**
 * @brief  设置双态按钮、复选框、单选框选中状态。
 * @param  target 控件名称或跨页面全路径。
 * @param  checked true 选中，false 取消。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_BinaryStateSet(const char *target, bool checked);

/**
 * @brief  设置进度条或滑块的当前数值。
 * @param  target 控件名称或跨页面全路径。
 * @param  value 数值内容。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_ProgressSetValue(const char *target, int32_t value);

/**
 * @brief  设置图片或页面背景图片资源 ID。
 * @param  target 控件名称或跨页面全路径。
 * @param  pictureId 图片资源 ID。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_PictureSetId(const char *target, uint16_t pictureId);

/**
 * @brief  设置系统变量 dim 当前亮度。
 * @param  brightness 亮度值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBrightness(uint16_t brightness);

/**
 * @brief  设置系统变量 dims 上电默认亮度。
 * @param  brightness 亮度值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBootBrightness(uint16_t brightness);

/**
 * @brief  设置当前波特率变量 baud。
 * @param  baudrate 波特率值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBaud(uint32_t baudrate);

/**
 * @brief  设置上电默认波特率变量 bauds。
 * @param  baudrate 波特率值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBootBaud(uint32_t baudrate);

/**
 * @brief  设置串口返回状态变量 bkcmd。
 * @param  mode 模式值，推荐 0-3。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetBkcmd(uint8_t mode);

/**
 * @brief  发送 sleep=1 使屏幕进入睡眠。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSleep(void);

/**
 * @brief  发送 sleep=0 使屏幕退出睡眠。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemWakeup(void);

/**
 * @brief  发送 randset 指令设置随机数范围。
 * @param  minValue 最小值。
 * @param  maxValue 最大值。
 * @retval HAL 状态值。
 */
HAL_StatusTypeDef TJC_SystemSetRandomRange(int32_t minValue, int32_t maxValue);

#endif
