# 淘晶驰串口屏驱动与协议解析库

本项目面向 **STM32 HAL 工程**，用于驱动淘晶驰（TJC）/ Nextion 类串口屏，并对串口屏返回数据进行标准化解析。

> 当前 AD9910 工程已将 TJC 屏配置为 `USART6`：`PC6/USART6_TX`
> 接屏幕 `RX`，`PC7/USART6_RX` 接屏幕 `TX`，115200 8N1。本文后续
> 出现的 `USART1` 代码仅是此通用驱动的接入示例，不代表当前工程配置。

当前版本已经将“串口收发”和“协议解析”拆分成两个层次：

- **驱动层**：负责 UART 发送、接收、中断 / DMA、环形缓冲区管理
- **协议层**：负责识别串口屏标准返回帧，完成结构化解析与异常处理

这样做的好处是：

1. 发送代码保持简单
2. 接收代码不再依赖手写 `if/else` 逐字节判断
3. 返回事件统一转换成结构体，业务代码更容易维护
4. 后续扩展新的协议类型时，不需要再改底层驱动

---

## 目录

- [1. 文件结构](#1-文件结构)
- [2. 功能概览](#2-功能概览)
- [3. 适用环境](#3-适用环境)
- [4. 快速接入](#4-快速接入)
- [5. CubeMX 配置步骤](#5-cubemx-配置步骤)
- [6. 头文件宏配置说明](#6-头文件宏配置说明)
- [7. 收发模式配置示例](#7-收发模式配置示例)
- [8. 标准接入代码示例](#8-标准接入代码示例)
- [9. 协议解析模块说明](#9-协议解析模块说明)
- [10. 消息类型说明](#10-消息类型说明)
- [11. API 说明](#11-api-说明)
- [12. 自定义业务处理示例](#12-自定义业务处理示例)
- [13. 异常处理机制说明](#13-异常处理机制说明)
- [14. 调试建议](#14-调试建议)
- [15. 常见问题](#15-常见问题)

---

## 1. 文件结构

当前库由以下 4 个核心文件组成：

```text
tjc_usart_hmi.h          驱动层头文件，负责宏配置与对外接口声明
tjc_usart_hmi.c          驱动层实现，负责 UART 收发、环形缓冲区、消息分发
tjc_protocol_parser.h    协议层头文件，定义返回消息类型、解析结果与辅助接口
tjc_protocol_parser.c    协议层实现，负责标准返回帧解析与异常帧处理
```

### 各文件职责

#### `tjc_usart_hmi.*`

负责：

- 启动 UART 接收
- 根据宏选择 TX / RX 的阻塞、中断、DMA 模式
- 接收字节写入环形缓冲区
- 调用协议解析器解析完整消息
- 将解析好的消息通过回调或轮询接口交给用户

#### `tjc_protocol_parser.*`

负责：

- 识别屏幕返回帧类型
- 解析固定长度返回帧
- 解析变长字符串返回帧
- 识别未知帧并尽量保留原始数据
- 处理异常帧、残缺帧、无结束符卡死等问题

---

## 2. 功能概览

当前版本支持：

- 独立串口屏驱动封装
- TX 模式独立配置：`阻塞 / 中断 / DMA`
- RX 模式独立配置：`中断 / DMA`
- 环形缓冲区缓存接收数据
- 对串口屏标准返回帧进行结构化解析
- 支持通过回调函数处理消息
- 支持通过“读取最近一条消息”的方式轮询处理
- 串口异常自动重启接收
- 未知帧与异常帧的兜底处理

---

## 3. 适用环境

### 软件环境

- STM32CubeMX / STM32CubeIDE 生成的 HAL 工程
- 工程中已经存在 `main.h`
- 已完成 UART 初始化

### 硬件环境

- STM32F407ZGT6（默认目标）
- 淘晶驰 / Nextion 串口屏

### 当前默认串口

默认使用：

```c
#define TJC_UART_HANDLE huart1
#define TJC_UART_INSTANCE USART1
```

如果你的项目不是 `USART1`，请改为你自己的串口句柄和串口实例。

---

## 4. 快速接入

如果你只想尽快跑通，请按下面顺序做。

### 第一步：把 4 个文件加入工程

```text
tjc_usart_hmi.h
tjc_usart_hmi.c
tjc_protocol_parser.h
tjc_protocol_parser.c
```

建议：

- `.h` 放到 `Core/Inc`
- `.c` 放到 `Core/Src`

### 第二步：包含头文件

在 `main.c` 或你的应用文件中加入：

```c
#include "tjc_usart_hmi.h"
```

### 第三步：UART 初始化后调用驱动初始化

```c
MX_USART1_UART_Init();
TJC_Init();
```

### 第四步：周期性调用解析函数

建议通过定时器周期调用：

```c
TJC_ParseIncomingData();
```

### 第五步：转发 HAL 回调

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  TJC_UART_RxCpltCallback(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  TJC_UART_ErrorCallback(huart);
}
```

### 第六步：注册消息回调或轮询读取消息

两种方式任选其一：

- 注册消息回调：`TJC_RegisterMessageHandler()`
- 轮询读取消息：`TJC_GetLastMessage()`

---

## 5. CubeMX 配置步骤

下面以默认 `USART1` 为例。

### 5.1 打开 UART 外设

在 CubeMX 中：

1. 打开芯片引脚视图
2. 选择 `USART1`
3. 模式选择 `Asynchronous`

### 5.2 配置串口参数

建议参数：

- Baud Rate：按屏幕工程配置，一般 `115200`
- Word Length：`8 Bits`
- Parity：`None`
- Stop Bits：`1`
- Hardware Flow Control：`None`
- Mode：`TX and RX`

### 5.3 配置中断

建议打开 UART 全局中断。

即使你使用 DMA，HAL 层仍然需要 UART / DMA 中断来更新状态与回调。

### 5.4 配置 DMA（当你启用 DMA 模式时）

#### RX 使用 DMA 时

- 添加一个 RX DMA 通道
- Direction：`Peripheral to Memory`
- Data Width：`Byte`
- Mode：建议 `Normal`

#### TX 使用 DMA 时

- 添加一个 TX DMA 通道
- Direction：`Memory to Peripheral`
- Data Width：`Byte`
- Mode：建议 `Normal`

### 5.5 生成代码后需要确认

请确认工程中已经存在：

1. `UART_HandleTypeDef huart1;`
2. `MX_USART1_UART_Init();`
3. 若启用 DMA，则对应 DMA 句柄与中断已经生成

---

## 6. 头文件宏配置说明

主要配置都在 `tjc_usart_hmi.h` 和 `tjc_protocol_parser.h` 中完成。

### 6.1 串口相关宏

```c
#define TJC_UART_HANDLE huart1
#define TJC_UART_INSTANCE USART1
```

作用：

- `TJC_UART_HANDLE`：指定 HAL 串口句柄
- `TJC_UART_INSTANCE`：指定对应外设实例

如果你改用 `USART6`，则改成：

```c
#define TJC_UART_HANDLE huart6
#define TJC_UART_INSTANCE USART6
```

### 6.2 缓冲区与协议宏

```c
#define TJC_RINGBUFFER_LEN 512U
#define TJC_RX_CHUNK_SIZE 1U
#define TJC_FRAME_LENGTH 7U
#define TJC_FRAME_HEADER 0x55U
#define TJC_FRAME_END_BYTE 0xFFU
```

说明：

- `TJC_RINGBUFFER_LEN`：驱动接收环形缓冲区长度
- `TJC_RX_CHUNK_SIZE`：每次硬件接收块长度，当前建议保持 `1`
- `TJC_FRAME_LENGTH / HEADER / END_BYTE`：发送协议相关基础参数

### 6.3 发送等待相关宏

```c
#define TJC_TX_TIMEOUT_MS 20U
#define TJC_TX_WAIT_TIMEOUT_MS 100U
#define TJC_ISR_TX_TIMEOUT_LOOPS 1000000UL
#define TJC_TX_PACKET_MAX_LEN 256U
```

说明：

- `TJC_TX_TIMEOUT_MS`：阻塞发送的 HAL 超时参数
- `TJC_TX_WAIT_TIMEOUT_MS`：IT / DMA 发送后的完成等待时间
- `TJC_ISR_TX_TIMEOUT_LOOPS`：在中断上下文中改用轮询发送时的最大等待循环次数
- `TJC_TX_PACKET_MAX_LEN`：驱动内部“正文 + 帧尾”一次拼包发送的最大缓存长度

补充说明：

- 当发送函数在 **普通线程上下文** 调用时，驱动仍按 `TJC_UART_TX_MODE` 选择阻塞 / IT / DMA 发送
- 当发送函数在 **中断上下文** 调用时，驱动会自动切换为 **寄存器轮询发送**，避免 `HAL_GetTick()` 不更新、异步完成中断无法抢占、串口状态长期 `BUSY` 等问题
- 这意味着：即使 `TJC_UART_TX_MODE` 配成 `DMA` 或 `IT`，在定时器中断里调用发送函数时，也不会再依赖异步发送完成回调

### 6.4 内置默认处理开关

```c
#define TJC_ENABLE_DEFAULT_HANDLER 1
```

当前版本驱动侧不再对接收消息做复杂默认业务处理，主要由上层回调处理。

保留这个宏主要是为了后续兼容扩展。

### 6.5 收发模式宏

```c
#define TJC_UART_MODE_BLOCKING 0U
#define TJC_UART_MODE_IT       1U
#define TJC_UART_MODE_DMA      2U
```

#### RX 模式

```c
#define TJC_UART_RX_MODE TJC_UART_MODE_DMA
```

可选：

- `TJC_UART_MODE_IT`
- `TJC_UART_MODE_DMA`

#### TX 模式

```c
#define TJC_UART_TX_MODE TJC_UART_MODE_DMA
```

可选：

- `TJC_UART_MODE_BLOCKING`
- `TJC_UART_MODE_IT`
- `TJC_UART_MODE_DMA`

### 6.6 协议解析层宏

这些宏位于 `tjc_protocol_parser.h`：

```c
#define TJC_PROTOCOL_STRING_MAX_LEN 128U
#define TJC_PROTOCOL_UNKNOWN_MAX_LEN 32U
#define TJC_PROTOCOL_STALL_THRESHOLD 128U
```

说明：

- `TJC_PROTOCOL_STRING_MAX_LEN`：保存字符串返回值时的最大长度
- `TJC_PROTOCOL_UNKNOWN_MAX_LEN`：未知帧保留原始字节的最大长度
- `TJC_PROTOCOL_STALL_THRESHOLD`：长时间未找到结束符时的保护阈值

---

## 7. 收发模式配置示例

### 7.1 TX DMA + RX DMA（默认推荐）

```c
#define TJC_UART_RX_MODE TJC_UART_MODE_DMA
#define TJC_UART_TX_MODE TJC_UART_MODE_DMA
```

适合：

- 屏幕交互频繁
- 项目已经配置好 DMA
- 希望减轻 CPU 开销

### 7.2 TX 阻塞 + RX DMA

```c
#define TJC_UART_RX_MODE TJC_UART_MODE_DMA
#define TJC_UART_TX_MODE TJC_UART_MODE_BLOCKING
```

适合：

- 接收频繁，发送量较小
- 想减少 TX 异步调试复杂度

### 7.3 TX DMA + RX 中断

```c
#define TJC_UART_RX_MODE TJC_UART_MODE_IT
#define TJC_UART_TX_MODE TJC_UART_MODE_DMA
```

### 7.4 TX 阻塞 + RX 中断

```c
#define TJC_UART_RX_MODE TJC_UART_MODE_IT
#define TJC_UART_TX_MODE TJC_UART_MODE_BLOCKING
```

这个模式最适合联调初期，最容易先跑通。

---

## 8. 标准接入代码示例

### 8.1 main.c 初始化示例

```c
#include "main.h"
#include "tjc_usart_hmi.h"

void App_TjcMessageHandler(const TJC_ProtocolMessage_t *message);

int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_TIM3_Init();

  TJC_Init();
  TJC_RegisterMessageHandler(App_TjcMessageHandler);
  HAL_TIM_Base_Start_IT(&htim3);

  TJC_SendText("t0", "txt", "System Ready");
  TJC_SendValue("n0", "val", 123);

  while (1) {
  }
}
```

### 8.2 定时器中断中解析接收数据

```c
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM3) {
    TJC_ParseIncomingData();
  }
}
```

建议周期：

- `5ms`
- `10ms`

### 8.3 UART 回调转发

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  TJC_UART_RxCpltCallback(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  TJC_UART_ErrorCallback(huart);
}
```

### 8.4 轮询读取最近一条消息示例

如果你不想注册回调，也可以在主循环中轮询：

```c
TJC_ProtocolMessage_t message;

while (1) {
  TJC_ParseIncomingData();

  if (TJC_GetLastMessage(&message)) {
    // 在这里处理最近一条消息
  }

  HAL_Delay(10);
}
```

---

## 9. 协议解析模块说明

新增的 `tjc_protocol_parser.*` 负责把屏幕原始返回字节流转成结构化消息。

### 解析器支持的核心返回类型

#### 1. bkcmd 执行结果类

例如：

- `00 FF FF FF` 无效指令
- `01 FF FF FF` 指令成功执行
- `02 FF FF FF` 控件 ID 无效
- `03 FF FF FF` 页面 ID 无效

解析后统一映射为：

```c
TJC_PROTOCOL_MSG_EXECUTE_RESULT
```

#### 2. 固定长度事件类

例如：

- `0x65` 控件点击事件
- `0x66` 当前页面 ID
- `0x67` 触摸坐标
- `0x68` 睡眠模式触摸事件
- `0x71` 数值变量返回
- `0x86/0x87/0x88/0x89/0xFD/0xFE`

#### 3. 变长字符串类

例如：

- `0x70 + ASCII内容 + FF FF FF`

#### 4. 未知帧

如果收到的帧不属于当前支持的标准格式，但包含合法结束符，解析器会尽量保留原始数据，并输出：

```c
TJC_PROTOCOL_MSG_UNKNOWN
```

这样你可以先抓到现场数据，再决定是否扩展协议支持。

---

## 10. 消息类型说明

解析完成后，你会在 `TJC_ProtocolMessage_t` 中拿到结构化结果。

### 当前主要消息类型

| 消息类型 | 说明 |
|---|---|
| `TJC_PROTOCOL_MSG_EXECUTE_RESULT` | bkcmd 执行结果 / 错误码 |
| `TJC_PROTOCOL_MSG_BUFFER_OVERFLOW` | 屏幕串口缓冲区溢出 |
| `TJC_PROTOCOL_MSG_TOUCH_EVENT` | 控件触摸事件 |
| `TJC_PROTOCOL_MSG_PAGE_ID` | 当前页面 ID |
| `TJC_PROTOCOL_MSG_TOUCH_COORD` | 普通触摸坐标 |
| `TJC_PROTOCOL_MSG_SLEEP_TOUCH` | 睡眠触摸事件 |
| `TJC_PROTOCOL_MSG_STRING_DATA` | 字符串返回值 |
| `TJC_PROTOCOL_MSG_NUMERIC_DATA` | 数值返回值 |
| `TJC_PROTOCOL_MSG_AUTO_SLEEP` | 自动睡眠 |
| `TJC_PROTOCOL_MSG_AUTO_WAKE` | 自动唤醒 |
| `TJC_PROTOCOL_MSG_STARTUP` | 系统启动成功 |
| `TJC_PROTOCOL_MSG_SD_UPGRADE` | 开始 SD 卡升级 |
| `TJC_PROTOCOL_MSG_TRANSPARENT_FINISHED` | 透传完成 |
| `TJC_PROTOCOL_MSG_TRANSPARENT_READY` | 透传准备完成 |
| `TJC_PROTOCOL_MSG_UNKNOWN` | 未知协议帧 |

### 结果码辅助说明函数

你可以直接使用：

```c
TJC_ProtocolIsErrorResult(code)
TJC_ProtocolResultCodeString(code)
TJC_ProtocolMessageTypeString(type)
```

这样可以减少你业务层里自己写一堆错误码映射表。

---

## 11. API 说明

### 驱动层 API

#### `TJC_Init`

```c
void TJC_Init(void);
```

作用：

- 清空接收缓冲区
- 启动接收流程
- 重置消息状态

#### `TJC_StartReceive`

```c
HAL_StatusTypeDef TJC_StartReceive(void);
```

作用：按当前 RX 模式重新启动一次接收。

#### `TJC_ParseIncomingData`

```c
void TJC_ParseIncomingData(void);
```

作用：

- 从环形缓冲区中拉取数据
- 调用协议解析器解析标准返回帧
- 解析成功后触发消息分发

#### `TJC_RegisterMessageHandler`

```c
void TJC_RegisterMessageHandler(TJC_MessageHandler_t handler);
```

作用：注册协议消息回调。

#### `TJC_GetLastMessage`

```c
bool TJC_GetLastMessage(TJC_ProtocolMessage_t *message);
```

作用：读取最近一次成功解析的消息，适合不想使用回调的场景。

### 发送类 API

#### `TJC_SendString`

发送原始命令字符串，并自动追加 `FF FF FF` 结束符。

```c
TJC_SendString("page 0");
TJC_SendString("click b0,1");
```

#### `TJC_SendText`

发送文本属性：

```c
TJC_SendText("t0", "txt", "Hello");
```

#### `TJC_SendValue`

发送数值属性：

```c
TJC_SendValue("n0", "val", 100);
```

#### `TJC_SendBytes`

发送自定义字节流，适合特殊指令封装。

### 协议层 API

#### `TJC_ProtocolTryParse`

这是协议层的核心解析函数，驱动内部已经调用，一般不需要应用层直接使用。

#### `TJC_ProtocolResultCodeString`

把执行结果码转成中文说明字符串。

#### `TJC_ProtocolMessageTypeString`

把消息类型转成中文名称。

---

## 12. 自定义业务处理示例

下面是一段比较典型的业务处理代码：

```c
void App_TjcMessageHandler(const TJC_ProtocolMessage_t *message)
{
  if (message == NULL) {
    return;
  }

  switch (message->type) {
    case TJC_PROTOCOL_MSG_EXECUTE_RESULT:
      if (message->payload.executeResult.isError) {
        // 可以在这里记录错误码
        // 例如：控件ID无效、页面ID无效、赋值失败等
      }
      break;

    case TJC_PROTOCOL_MSG_TOUCH_EVENT:
      if (message->payload.touchEvent.touchEvent == TJC_TOUCH_PRESS) {
        // 某控件被按下
      }
      break;

    case TJC_PROTOCOL_MSG_PAGE_ID:
      // 当前页面编号
      break;

    case TJC_PROTOCOL_MSG_STRING_DATA:
      // 读取字符串返回值
      break;

    case TJC_PROTOCOL_MSG_NUMERIC_DATA:
      // 读取数值返回值
      break;

    case TJC_PROTOCOL_MSG_UNKNOWN:
      // 收到了暂未支持的帧，可以抓包分析后再扩展协议层
      break;

    default:
      break;
  }
}
```

嵌入工程示例：

```c
#include "tjc_usart_hmi.h"  // 必须包含驱动头文件

/**
 * @brief TJC Protocol Message Handler
 * @details Processes protocol messages from the TJC screen module, including
 *          command execution results, buffer overflow events, touch events,
 *          page switching, and data responses. This handler can be implemented
 *          in main.c or a dedicated business logic file.
 * @param message Pointer to the protocol message structure
 * @return void
 */
void App_TJC_MessageHandler(const TJC_ProtocolMessage_t *message) {
  // 安全判断
  if (message == NULL) {
    return;
  }

  // 根据消息类型处理
  switch (message->type) {
  // 1. 指令执行结果（成功/失败）
  case TJC_PROTOCOL_MSG_EXECUTE_RESULT: {
    uint8_t code = message->payload.executeResult.resultCode;
    bool isErr = message->payload.executeResult.isError;

    if (isErr) {
      // 你可以在这里加错误处理：重试、报警灯等
    } else {
    }
    break;
  }

  // 2. 缓冲区溢出
  case TJC_PROTOCOL_MSG_BUFFER_OVERFLOW: {
    // 你可以在这里加溢出处理：清空缓冲、报警灯等
    break;
  }

  // 3. 控件触摸事件（按钮、开关等）
  case TJC_PROTOCOL_MSG_TOUCH_EVENT: {
    uint8_t pageId = message->payload.touchEvent.pageId;
    uint8_t compId = message->payload.touchEvent.componentId;
    uint8_t event = message->payload.touchEvent.touchEvent;

    // ==================== 你的业务逻辑写在这里 ====================
    if (event == TJC_TOUCH_PRESS) {
      // 按钮按下
      if (pageId == 0 && compId == 0) {
        // 页面0，控件0（b0）按下 → 执行你的业务
      }
      if (pageId == 0 && compId == 1) {
        // 页面0，控件1（b1）按下 → 执行你的业务
      }
    } else {
      // 按钮松开
    }
    // ============================================================
    break;
  }

  // 4. 当前页面ID（页面切换）
  case TJC_PROTOCOL_MSG_PAGE_ID: {
    uint8_t pageId = message->payload.pageInfo.pageId;

    // 页面切换后初始化逻辑
    break;
  }

  // 5. 坐标触摸（点击屏幕任意位置）
  case TJC_PROTOCOL_MSG_TOUCH_COORD: {
    uint16_t x = message->payload.touchCoord.x;
    uint16_t y = message->payload.touchCoord.y;
    uint8_t evt = message->payload.touchCoord.touchEvent;
    break;
  }

  // 6. 休眠唤醒触摸
  case TJC_PROTOCOL_MSG_SLEEP_TOUCH: {

    break;
  }

  // 7. 字符串数据返回（如 t0.txt 读取返回）
  case TJC_PROTOCOL_MSG_STRING_DATA: {

    break;
  }

  // 8. 数值数据返回（如 n0.val 读取返回）
  case TJC_PROTOCOL_MSG_NUMERIC_DATA: {
    uint32_t val = message->payload.numericData.value;

    break;
  }

  // 9. 自动休眠
  case TJC_PROTOCOL_MSG_AUTO_SLEEP: {

    break;
  }

  // 10. 自动唤醒
  case TJC_PROTOCOL_MSG_AUTO_WAKE: {

    break;
  }

  // 11. 屏幕启动完成
  case TJC_PROTOCOL_MSG_STARTUP: {

    break;
  }

  // 12. SD卡升级
  case TJC_PROTOCOL_MSG_SD_UPGRADE: {

    break;
  }

  // 13. 透传完成
  case TJC_PROTOCOL_MSG_TRANSPARENT_FINISHED: {

    break;
  }

  // 14. 透传准备就绪
  case TJC_PROTOCOL_MSG_TRANSPARENT_READY: {

    break;
  }

  // 15. 未知帧
  case TJC_PROTOCOL_MSG_UNKNOWN: {

    break;
  }

  default:
    break;
  }
}
```

典型注册流程：
```c
int main(void)
{
    // 1. 系统初始化（HAL、串口、定时器等）
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();  // 串口初始化
    MX_TIM3_Init();         // 定时器初始化（用于周期解析数据）

    // 2. 驱动初始化
    TJC_Init();

    // 3. 注册自定义业务处理函数（核心步骤）
    TJC_RegisterMessageHandler(App_TjcMessageHandler);

    // 4. 启动定时器（用于周期调用 TJC_ParseIncomingData）
    HAL_TIM_Base_Start_IT(&htim3);

    // 5. 发送测试指令
    TJC_SendText("t0", "txt", "系统就绪");

    // 主循环
    while (1) {
        // 其他业务逻辑...
    }
}
```

---

## 13. 异常处理机制说明

这次重构的重点之一，就是把接收异常处理做得更稳一些。

### 13.1 串口硬件异常

当 HAL 上报 UART 错误时：

- 驱动会设置“需要重启接收”的标志
- 在下一次 `TJC_ParseIncomingData()` 调用时自动尝试恢复接收

### 13.2 环形缓冲区溢出

当 MCU 本地环形缓冲区满时：

- 新收到的字节会被丢弃
- 内部会累计本地溢出计数

这种情况通常意味着：

- 上层调用 `TJC_ParseIncomingData()` 太慢
- 串口返回过于频繁
- 缓冲区长度设置过小

### 13.3 屏幕返回 `24 FF FF FF`

这表示 **串口屏内部缓冲区溢出**，和 MCU 本地缓冲区溢出不是一回事。

解析器会把它识别成：

```c
TJC_PROTOCOL_MSG_BUFFER_OVERFLOW
```

### 13.4 无效帧 / 错位帧

解析器在遇到异常数据时，不会直接卡死：

- 固定长度帧结束符不匹配时，会丢弃部分字节并继续同步
- 变长字符串帧长时间找不到结束符时，会触发保护性丢弃，避免缓冲区一直被错误数据占满
- 未识别但包含合法结束符的帧，会作为 `UNKNOWN` 帧保留下来

这几条就是当前“异常处理机制完善”的核心。

---

## 14. 调试建议

### 14.1 第一次移植建议用最简单模式先跑通

```c
#define TJC_UART_RX_MODE TJC_UART_MODE_IT
#define TJC_UART_TX_MODE TJC_UART_MODE_BLOCKING
```

等发送、接收、回调都确认没问题，再切换到 DMA。

### 14.2 优先验证发送链路

先发一句最简单的命令：

```c
TJC_SendText("t0", "txt", "Hello");
```

如果屏幕能显示文字，说明：

- 硬件连接基本正确
- UART 发送链路正常

### 14.3 再验证接收链路

接收联调建议顺序：

1. 先确保回调被触发
2. 再确保 `TJC_ParseIncomingData()` 在周期调用
3. 再观察是否能收到结构化消息
4. 最后再做业务逻辑

### 14.4 调试时优先看这几个点

- `TJC_UART_HANDLE` / `TJC_UART_INSTANCE` 是否和工程一致
- `HAL_UART_RxCpltCallback()` 是否有转发
- `HAL_UART_ErrorCallback()` 是否有转发
- `TJC_ParseIncomingData()` 是否按周期调用
- 屏幕是否真的会返回对应类型的数据

---

## 15. 常见问题

### 15.1 屏幕完全没反应

先查：

1. TX / RX 是否接反
2. GND 是否共地
3. 波特率是否与屏幕工程一致
4. 串口宏是否改对
5. 屏幕工程是否已经下载到屏上

### 15.2 发送正常，接收没有任何消息

先查：

1. 是否实现了 `HAL_UART_RxCpltCallback()`
2. 是否在回调中调用了 `TJC_UART_RxCpltCallback()`
3. `TJC_ParseIncomingData()` 是否在定时器或主循环中被调用
4. 屏幕当前操作是否真的会触发返回数据

### 15.3 DMA 模式不工作

先查：

1. CubeMX 中是否真的配置了 RX DMA / TX DMA
2. DMA 方向是否正确
3. UART 中断和 DMA 中断是否开启
4. `TJC_UART_RX_MODE / TJC_UART_TX_MODE` 是否与 CubeMX 配置一致

### 15.4 为什么会收到 `UNKNOWN` 消息

一般有三种可能：

1. 这是屏幕文档里有、但当前解析器还没实现的新类型
2. 数据流中夹杂了异常数据
3. 上位机 / 单片机发送节奏太快，导致帧错位

如果你能稳定复现某种 `UNKNOWN` 帧，建议先把它的原始字节打印出来，再补充协议解析器支持。

### 15.5 为什么会出现执行失败结果码

例如：

- 控件名错误
- 页面名错误
- 变量赋值格式错误
- 参数数量错误

你可以直接调用：

```c
TJC_ProtocolResultCodeString(code)
```

来拿到中文说明，避免自己查表。

---

## 结语

如果你的目标是“项目先稳定落地”，建议顺序如下：

1. 先确认发送能用
2. 再确认接收回调能进
3. 再确认协议解析能正常出结构体消息
4. 最后才把业务逻辑完整接上

当前这套结构已经把“底层收发”和“协议解析”分开，后续如果你要继续新增更多返回帧支持，直接扩展 `tjc_protocol_parser.c` 就可以，不需要再去改底层串口驱动。
