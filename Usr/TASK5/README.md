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

button EXTI -> press counters -> 50 ms debounce / 400 ms click window
            -> Task5_Start

main loop -> Task5_Process -> UART TX ring / DAC / DDS / PLL port
```

UART ISR 不解析协议、不执行 SPI、不做浮点搜索。DAC DMA 使用无完成中断
的循环搬运，避免 10 kHz 锯齿波产生 10 kHz 的周期完成中断。

OpenMV 端联调协议见：

```text
ref/Task5与OpenMV串口通信协议-最终版.md
```
