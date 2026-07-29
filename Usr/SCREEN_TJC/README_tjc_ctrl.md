# TJC 控件支持库使用说明

本文档对应新增的上层控件支持库：

```text
tjc_ctrl.h
tjc_ctrl.c
```

该库建立在现有 `tjc_usart_hmi.*` 驱动之上，目的是把常用的控件操作、系统变量操作和基础指令操作进一步封装成更容易调用的函数接口。

> 说明：本文件是新增控件支持库的专用说明文档，**不会替换或修改现有 README.md**。

---

## 1. 设计目标

这个控件支持库主要解决三个问题：

1. 避免在业务代码里到处手写字符串命令
2. 让控件操作更接近“函数调用”而不是“手拼指令”
3. 在不修改现有底层驱动文件的前提下，增加更高层、更可维护的控件操作接口

---

## 2. 与现有驱动的关系

### 2.1 分层关系

当前项目推荐按三层理解：

```text
业务层
  ↓
tjc_ctrl.*               控件支持层（本次新增）
  ↓
tjc_usart_hmi.*          串口驱动层
  ↓
tjc_protocol_parser.*    协议解析层
```

### 2.2 不修改旧库

本次新增库：

- **不修改** `tjc_usart_hmi.c`
- **不修改** `tjc_usart_hmi.h`
- 仅新增自己的 `.c/.h` 和单独文档

因此不会破坏你现有已经完成的驱动结构。

---

## 3. 只包含一个头文件的使用方式

为了满足“使用时还是只用调用一个头文件”的要求：

```c
#include "tjc_ctrl.h"
```

即可。

原因是：

`tjc_ctrl.h` 内部已经包含了：

```c
#include "tjc_usart_hmi.h"
```

所以你在应用层中只需要包含 `tjc_ctrl.h`，不需要再手动包含旧驱动头文件。

---

## 4. 新增文件说明

### `tjc_ctrl.h`

对外头文件，包含：

- 控件操作函数声明
- 系统变量操作函数声明
- 基础指令封装声明
- `prints` 格式宏定义

### `tjc_ctrl.c`

实现文件，负责：

- 文本转义
- 指令格式化
- 高层函数调用到底层 `TJC_SendString()`

---

## 5. 适合封装的对象范围

根据 `.doc/` 中的说明，这次上层控件支持库优先封装以下高频能力：

### 5.1 控件属性写入

- `txt`
- `val`
- `font`
- `pic`
- 任意原始属性表达式

### 5.2 控件行为操作

- `vis`
- `tsw`
- `click`
- `prints`
- `get`
- 曲线控件 `add / addt / cle`

### 5.3 页面操作

- 页面名称跳转
- 页面 ID 跳转

### 5.4 系统变量操作

- `dim`
- `dims`
- `baud`
- `bauds`
- `bkcmd`
- `sleep`
- `randset`

### 5.5 常用控件封装

- 文本控件
- 数字 / 数值类控件
- 按钮控件
- 双态按钮 / 复选框 / 单选框
- 进度条 / 滑块
- 图片控件

---

## 6. 快速开始

### 第一步：把文件加入工程

把下面两个文件加入工程：

```text
tjc_ctrl.h
tjc_ctrl.c
```

### 第二步：在应用代码中只包含一个头文件

```c
#include "tjc_ctrl.h"
```

### 第三步：保证底层驱动已初始化

仍然按原有方式初始化：

```c
MX_USART1_UART_Init();
TJC_Init();
```

### 第四步：直接调用控件支持函数

```c
TJC_TextSetText("t0", "Hello");
TJC_ValueSet("n0", 123);
TJC_ComponentSetVisible("t0", true);
TJC_ComponentSetTouch("b0", true);
```

---

## 7. 常用函数总览

### 7.1 基础通用接口

#### `TJC_CtrlSendCommand`

发送一条完整原始指令：

```c
TJC_CtrlSendCommand("page main");
```

#### `TJC_CtrlSetValue`

写入数值属性：

```c
TJC_CtrlSetValue("n0", "val", 100);
TJC_CtrlSetValue("j0", "val", 80);
```

#### `TJC_CtrlSetText`

写入文本属性：

```c
TJC_CtrlSetText("t0", "txt", "Hello World");
```

#### `TJC_CtrlSetRaw`

写入原始表达式，不自动加引号：

```c
TJC_CtrlSetRaw("n0", "val", "n1.val+n2.val");
```

适合做表达式赋值、跨控件运算等场景。

---

### 7.2 页面控制

#### `TJC_PageSet`

按页面名称跳转：

```c
TJC_PageSet("main");
```

#### `TJC_PageSetById`

按页面 ID 跳转：

```c
TJC_PageSetById(0);
```

---

### 7.3 控件显示与触摸

#### `TJC_ComponentSetVisible`

```c
TJC_ComponentSetVisible("t0", true);   // 显示
TJC_ComponentSetVisible("t0", false);  // 隐藏
```

#### `TJC_ComponentSetTouch`

```c
TJC_ComponentSetTouch("b0", true);   // 启用触摸
TJC_ComponentSetTouch("b0", false);  // 禁用触摸
```

#### `TJC_ComponentClick`

```c
TJC_ComponentClick("m0", true);   // 触发按下事件
TJC_ComponentClick("m0", false);  // 触发弹起事件
```

---

### 7.4 串口输出与获取变量

#### `TJC_ComponentPrint`

```c
TJC_ComponentPrint("n0.val", TJC_PRINTS_FORMAT_DEC_OR_TEXT);
TJC_ComponentPrint("t0.txt", TJC_PRINTS_FORMAT_DEC_OR_TEXT);
```

输出格式宏：

```c
TJC_PRINTS_FORMAT_DEC_OR_TEXT
TJC_PRINTS_FORMAT_HEX_1BYTE
TJC_PRINTS_FORMAT_HEX_2BYTE
TJC_PRINTS_FORMAT_HEX_4BYTE
```

#### `TJC_ComponentGet`

请求屏幕返回某个变量的值：

```c
TJC_ComponentGet("t0.txt");
TJC_ComponentGet("n0.val");
```

返回值通过现有协议解析层接收。

如果你希望在调用后直接把结果回填到 MCU 内部变量，可以使用新增的同步读取接口：

```c
int32_t numberValue;
uint32_t progressValue;
float virtualFloatValue;

TJC_ComponentGetInt32("n0.val", &numberValue, 0);
TJC_ComponentGetUInt32("j0.val", &progressValue, 0);
TJC_ComponentGetFloat("x0.val", 2, &virtualFloatValue, 0);
```

说明：

- 最后一个超时参数填 `0` 时，内部使用默认超时 `TJC_CTRL_GET_TIMEOUT_MS`
- `TJC_ComponentGetFloat()` 的 `2` 对应虚拟浮点数控件的 `vvs1=2`
- 例如屏幕返回原始值 `314`，且 `vvs1=2` 时，函数会回填 `3.14f`
- 这些同步接口内部会主动轮询 `TJC_ParseIncomingData()` 并消费最近一条返回消息，因此更适合“单次请求、单次读取”的场景

---

### 7.4 曲线控件接口

#### `TJC_CurveAddValue`

向指定曲线通道追加 1 个数据点：

```c
TJC_CurveAddValue("s0", 0, 123);
TJC_CurveAddValue("main.s0", 1, 80);
```

说明：

- `channel` 范围为 `0~3`
- `value` 范围为 `0~255`
- 实际使用时还应保证数据值不超过曲线控件显示高度

#### `TJC_CurveStartPassThrough`

向指定通道透传一段原始字节数据：

```c
uint8_t waveData[100];

for (uint16_t i = 0; i < 100U; i++) {
  waveData[i] = (uint8_t)(i & 0xFFU);
}

TJC_CurveStartPassThrough("s0", 0, waveData, 100U);
```

说明：

- 该接口内部会依次发送 `addt` 指令、原始数据流，以及透传结束标记 `0x01`（并自动附加协议结束符）
- `addt` 指令不支持跨页面目标，因此 `target` 仅支持当前页控件名（例如 `s0`）

#### 两阶段透传（推荐用于严格时序）

当需要在收到透传就绪消息后再发数据时，可使用两阶段接口：

```c
TJC_CurvePreparePassThrough("s0", 0, 100U);

if (TJC_CurveWaitPassThroughReady(0U) == HAL_OK) {
  TJC_CurveSendPassThroughData(waveData, 100U);
}
```

说明：

- `TJC_CurvePreparePassThrough()` 仅发送 `addt` 指令
- `TJC_CurveWaitPassThroughReady()` 等待屏幕返回透传就绪消息（`0xFE`）
- `TJC_CurveSendPassThroughData()` 发送原始数据并自动补发透传结束序列

#### `TJC_CurveClearChannel`

清除指定通道中的数据：

```c
TJC_CurveClearChannel("s0", 0);
```

---

## 8. 针对常见控件的快捷接口

### 8.1 文本控件

#### 设置文本

```c
TJC_TextSetText("t0", "欢迎使用");
```

#### 设置字体

```c
TJC_TextSetFont("t0", 1);
```

---

### 8.2 数值类控件

适用于：

- 数字控件
- 数值变量控件
- 虚拟浮点数控件（底层仍然通过 `val` 操作）

```c
TJC_ValueSet("n0", 100);
TJC_ValueSet("va0", 256);
```

---

### 8.3 按钮控件

```c
TJC_ButtonSetText("b0", "启动");
```

---

### 8.4 双态按钮 / 复选框 / 单选框

```c
TJC_BinaryStateSet("bt0", true);
TJC_BinaryStateSet("c0", false);
TJC_BinaryStateSet("r0", true);
```

这些控件本质都可以通过 `val=0/1` 控制状态。

---

### 8.5 进度条 / 滑块

```c
TJC_ProgressSetValue("j0", 50);
TJC_ProgressSetValue("h0", 80);
```

---

### 8.6 图片 / 页面背景图片

```c
TJC_PictureSetId("p0", 3);
TJC_PictureSetId("main", 1);
```

注意：如果是页面背景图片，目标对象应当是页面控件名称，并且该属性确实支持 `pic`。

---

## 9. 系统变量接口

### 当前亮度

```c
TJC_SystemSetBrightness(80);
```

### 上电默认亮度

```c
TJC_SystemSetBootBrightness(100);
```

### 当前波特率

```c
TJC_SystemSetBaud(115200);
```

### 上电默认波特率

```c
TJC_SystemSetBootBaud(115200);
```

### 串口返回状态设置

```c
TJC_SystemSetBkcmd(3);
```

### 睡眠 / 唤醒

```c
TJC_SystemSleep();
TJC_SystemWakeup();
```

### 随机数范围

```c
TJC_SystemSetRandomRange(1, 100);
```

---

## 10. 跨页面操作说明

根据 `.doc/04_书写语法.md`，跨页面访问全局控件时可以直接使用全路径。

### 示例

```c
TJC_TextSetText("main.t0", "跨页面文本");
TJC_CtrlSetValue("page1.n0", "val", 88);
```

### 注意事项

1. 被跨页面操作的控件必须设置为 **全局**
2. 路径格式为：

```text
页面名.控件名
```

例如：

```text
main.t0
page1.n0
```

---

## 11. 文本转义说明

由于串口屏的 `txt` 属性赋值需要遵循特殊语法，因此 `TJC_CtrlSetText()` 和基于它封装的文本接口，内部会自动处理以下字符转义：

- `"`
- `\`
- `\r`
- `\n`

例如：

```c
TJC_TextSetText("t0", "hello\rworld");
TJC_TextSetText("t0", "hello\"world");
TJC_TextSetText("t0", "hello\\world");
```

因此业务层不需要再手工拼接复杂的转义命令。

---

## 12. 推荐使用方式

### 方式一：业务代码优先使用快捷接口

例如：

```c
TJC_TextSetText("t0", "Ready");
TJC_ValueSet("n0", 123);
TJC_ProgressSetValue("j0", 50);
```

### 方式二：遇到未封装属性时退回通用接口

例如某些特殊属性未在快捷函数中单独封装，可以这样写：

```c
TJC_CtrlSetValue("main", "pic", 2);
TJC_CtrlSetRaw("n0", "val", "n1.val+n2.val");
```

### 方式三：遇到特殊指令时直接发送原始命令

```c
TJC_CtrlSendCommand("touch_j");
TJC_CtrlSendCommand("rest");
```

---

## 13. 与现有库的兼容性说明

本新增库：

- 不会替换现有驱动函数
- 不会修改现有驱动文件
- 所有新增接口均以 `TJC_` 前缀命名，但名称与现有函数不冲突
- 内部最终仍然调用现有底层发送接口

因此可以直接并入当前工程，不影响你现有已写好的收发与协议解析逻辑。

---

## 14. 什么时候适合继续扩展这个库

如果你后续要继续扩展，建议优先添加：

1. 曲线控件（`add` / `cle` / `addt`）高层封装
2. 掉电存储（`wepo` / `repo`）高层封装
3. RTC 时间操作封装
4. 页面背景、图层、移动等控件布局封装

目前这版已经足够覆盖大多数“上位机页面控件显示 + MCU 业务控制”的常用场景。

---

## 15. 总结

如果你的目标是：

- 尽量少写字符串命令
- 保持代码可读性
- 在不动现有底层驱动的前提下增加控件操作能力

那么直接在应用层只包含：

```c
#include "tjc_ctrl.h"
```

然后使用 `TJC_TextSetText()`、`TJC_ValueSet()`、`TJC_ComponentSetVisible()`、`TJC_SystemSetBkcmd()` 这一类函数，就是最推荐的方式。
