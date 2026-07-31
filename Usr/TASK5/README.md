# Task5 implementation

本目录实现 STM32 侧的 OpenMV 协议和锯齿波端口：

| 文件 | 职责 |
|---|---|
| `openmv_protocol.*` | 纯 C 帧编解码、CRC16/MODBUS、流式解析 |
| `openmv_uart.*` | UART5 单字节中断、RX/TX 环形缓冲、主循环发布帧 |
| `task5_controller.*` | Session/Test、ACK、重发、粗识别和 DDS 搜索状态机 |
| `app_saw.*` | PA4 DAC、TIM6 TRGO、DMA1 Stream5 的 100 点单增锯齿波 |

执行路径：

```text
UART5 ISR -> RX byte ring -> OpenMV_UART_Service -> frame parser
           -> Task5_OnFrame -> fixed-size Task5 state

button EXTI -> press counters -> 50 ms release debounce / 400 ms hold timer
            -> Task5_Start

main loop -> Task5_Process -> UART TX ring / DAC / DDS / PLL port
                            -> PE6 output select (low=DAC, high=DDS)
```

UART ISR 不解析协议、不执行 SPI、不做浮点搜索。DAC DMA 使用无完成中断
的循环搬运，避免 10 kHz 锯齿波产生 10 kHz 的周期完成中断。

Task5 的三个模式键在等待、通信、DDS 搜索、本地锁相、已锁定和错误状态
下都可重新选择。重新选择时 `Task5_Start()` 先把旧 Session 的
`STOP_TASK(reason=0x04)` 放入 UART5 TX 环，再以递增的新 Session ID
排入 `START_TASK`；随后停止旧输出并按新选择重新启动 DAC。旧 Session
迟到的 ACK 或结果不会推进新状态机。只有 Task1–4 状态关闭按键 EXTI；
其他非 Task5 状态即使产生按键计数，也不会启动 Task5 Session。

粗识别等待不再以 5 秒为失败期限。收到 START ACK 后，STM32 保持 DAC
输出并持续等待，每 5 秒重放原 START_TASK（TYPE、SEQ、Session 和
Payload 均不变），使 OpenMV 能重发可能丢失的结果。旧 OpenMV 若返回
失败结果或零倍数，STM32 会 ACK、发送 STOP 并用新 Session 自动重启
相同模式，不进入 `OpenMV bad result` 错误；用户可随时按模式键取消重选。

DAC 表固定覆盖全部 12 bit 码域：第一个点为 0，最后一个点为 4095，
初始化时会校验这两个端点。因此这里的“满幅”指数字码满量程；PA4 的实际
电压上下限仍取决于 VDDA/VREF+、DAC 缓冲器压降和外部负载。

为便于现场调试，Task5 内部错误不会切换到全局错误页，也不会立即关闭
输出。状态机会锁存在 Task5 错误态，取消当前协议重试，并重新启动本次
选择的 1 kHz 或 10 kHz 满码域 DAC 锯齿波，同时保持 PE6 低电平选择
DAC。屏幕固定显示：

```text
ERR: <具体原因>
OUTPUT ACTIVE/OFF
RX<合法帧数> CRC<CRC错误数> U<UART硬件错误数>
```

其中 `OUTPUT ACTIVE` 表示错误后调试输出仍在运行；屏幕不会显示所选
DAC 锯齿波频率。用户明确退出 Task5 时会尽力发送
`STOP_TASK(reason=0x01)`、停止 DAC，并恢复 PE6 高电平的安全/DDS
路径；错误态重新选择模式时则发送 `STOP_TASK(reason=0x04)` 并立即开始
新 Session。

OpenMV 端联调协议见：

```text
ref/Task5与OpenMV串口通信协议-最终版.md
```
