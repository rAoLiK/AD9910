# Task5 implementation

本目录实现 STM32 侧的 OpenMV 协议和锯齿波端口：

| 文件 | 职责 |
|---|---|
| `openmv_protocol.*` | 纯 C 帧编解码、CRC16/MODBUS、流式解析 |
| `openmv_uart.*` | UART5 单字节中断、RX/TX 环形缓冲、主循环发布帧 |
| `task5_controller.*` | Session/Test、ACK、重发、粗识别、视觉锁频及本地 PLL 交接状态机 |
| `visual_lock.*` | 纯 C 的 0.1 Hz 视觉 PI、方向试探/反转、±5 Hz 保护与直线对相 |
| `app_saw.*` | PA4 DAC、TIM6 TRGO、DMA1 Stream5 的 100 点单增锯齿波 |

执行路径：

```text
UART5 ISR -> RX byte ring -> OpenMV_UART_Service -> frame parser
           -> Task5_OnFrame -> fixed-size Task5 state

button EXTI -> press counters -> 50 ms release debounce / 400 ms hold timer
            -> Task5_Start

main loop -> Task5_Process -> UART TX ring / DAC / DDS / TJC HMI port
                            -> PE6 output select (low=DAC, high=DDS)
```

UART ISR 不解析协议、不执行 SPI、不做浮点搜索。DAC DMA 使用无完成中断
的循环搬运，避免 10 kHz 锯齿波产生 10 kHz 的周期完成中断。

Task5 的三个模式键在等待、通信、DDS 步进搜索、视觉锁定、STOP 握手、本地锁相和错误状态
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

## 粗估后的 100 Hz 步进搜索

粗识别得到目标 DDS 的大致频率后，F407 先按就近取整把它量化到 100 Hz
频率网格并限制在合法输出范围，记为 `origin`。第二阶段不再按比例放大步长，
也不使用高/低方向或二分搜索，
而是始终按下面的顺序测试候选频率：

```text
origin,
origin + 100 Hz, origin - 100 Hz,
origin + 200 Hz, origin - 200 Hz,
origin + 300 Hz, origin - 300 Hz, ...
```

越出 DDS 合法输出范围的候选不应下发。每次设置 DDS 并等待输出稳定后，
F407 发送一个新的 `DDS_TEST`。OpenMV 不判断频率偏高还是偏低；它只检查
固定示波器屏幕 ROI 内的绿色轨迹是否同时满足经标定的多帧稳定性和稀疏度
条件，并在图像可用时只返回以下两种业务结果：

- `TARGET_REACHED`（简称 `TARGET`）：当前候选呈现目标李萨如图形；
- `NOT_MATCHED`：当前候选不满足目标条件，F407 转到序列中的下一个频率。

若整个观察期限内无法定位屏幕或取得足够有效轨迹帧，OpenMV 返回
`IMAGE_ERROR`，F407 在同一候选频率重试，避免把相机故障误当成
`NOT_MATCHED` 后继续走频。
当前最多测试 41 个不同候选（非边界情况下覆盖 `origin ± 2000 Hz`）；同一
候选的图像错误重试不占用这 41 个候选名额。

`TARGET_REACHED` 只表示进入精锁范围，不表示 Task5 已完成。F407 随即发送
`VISUAL_LOCK_START`，所有模式都先以输入频率的一倍、正斜率直线为视觉目标。
OpenMV 以最高可用帧率连续发送折叠相位、相位变化速度、质量和图形有效标志；
遥测不要求 ACK，新帧可以覆盖丢失的旧帧，视觉锁定期间暂停 OLED 刷新。

F407 先比较原频率、`+0.1 Hz` 和必要时的 `-0.1 Hz` 探针来确定调节方向，
随后根据相位变化速度作 0.1 Hz 网格的 PI 调节。连续变差时恢复最佳点并反向；
任何方向若将越过初始命中频率的 `±5 Hz`，立即恢复初始频率并反向。频率稳定
后再调整 DDS 相位字，使图像收敛为一倍正斜率直线并连续稳定。

由于 400 MHz 系统时钟下单个 AD9910 FTW 约为 0.093 Hz，应用层以 1 ms
节拍在相邻两个 FTW 间作脉冲密度调制，使 0.1 Hz 指令获得正确的长期平均频率。
视觉直线稳定后才发送 `STOP_TASK`；STOP ACK 正常到达或重试耗尽后都会立即
按用户模式切换频率/相位目标并启动 STM32 本地 ADC+AD9910 PLL。二倍频模式
只在此交接点把频率乘二。只有本地 PLL 报告最终锁定后才发送一次 `beep 500`，
视觉命中、视觉直线锁定和 STOP 握手阶段均不响铃。

视觉阈值必须在实际相机位置、曝光、示波器亮度和接线条件下，用“频率正确”、
`+100 Hz`、`-100 Hz`、`+200 Hz`、`-200 Hz` 等多帧数据标定并留出验证集。
100 Hz 网格和稳定李萨如判定只是搜索设计，不能在未经上板实测、并用频率计
或示波器核验前，把 `TARGET_REACHED` 绝对表述为 DDS 与信号源误差已经小于
1 Hz。

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
openmv/Task5与OpenMV串口通信协议-最终版.md
```

## Host regression checks

```powershell
gcc -std=c11 -Wall -Wextra -Werror -IUsr\TASK5 analysis\test_visual_lock.c Usr\TASK5\visual_lock.c -lm -o build\host-tests\test_visual_lock.exe
.\build\host-tests\test_visual_lock.exe
gcc -std=c11 -Wall -Wextra -Werror -IUsr\TASK5 analysis\test_task5_controller.c Usr\TASK5\task5_controller.c Usr\TASK5\openmv_protocol.c Usr\TASK5\visual_lock.c -o build\host-tests\test_task5_controller.exe
.\build\host-tests\test_task5_controller.exe
python analysis\test_lissajous_stability.py
```

最后一条测试从 OpenMV 单文件的 AST 中只加载稳定判定器，并用合成掩膜验证
最小观察跨度、最新帧稳定、丢帧打断连续性以及 `IMAGE_ERROR` 回退；相机取图、
阈值和实际李萨如图形仍必须在 N6 与真实示波器上验证。
