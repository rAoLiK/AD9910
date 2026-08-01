# Task5 实现说明

本目录实现 STM32 侧 OpenMV 协议、绝对频率搜索与永久视觉锁相。

| 文件 | 职责 |
|---|---|
| `openmv_protocol.*` | 帧编解码、CRC16/MODBUS、流式解析 |
| `openmv_uart.*` | USART2 中断收发环形缓冲与主循环服务 |
| `task5_controller.*` | Session/Test、可靠命令、单倍绝对频率搜索、视觉保持状态机 |
| `visual_lock.*` | 基于图像运动速度的永久 DDS 频率闭环，硬限制 ±5 Hz |
| `app_saw.*` | PA4 DAC、TIM6 TRGO、DMA1 Stream5 的满码锯齿波 |

## 执行结构

```text
USART2 ISR -> RX ring -> OpenMV_UART_Service -> Task5_OnFrame
button EXTI -> debounce/hold event -> Task5_Start / Task5_Exit
main loop -> Task5_Process -> UART TX / DAC / DDS / TJC HMI
```

ISR 不解析协议、不执行 DDS SPI、不做浮点图像控制。所有状态转换和硬件命令
都在主循环完成。

## 找频与响铃

粗识别后，F407 将估计值量化到 100 Hz 网格并按中心向两侧搜索：

```text
origin,
origin + 100 Hz, origin - 100 Hz,
origin + 200 Hz, origin - 200 Hz, ...
```

三种题型在整个找频阶段都只测试单倍 DDS，绝不为“∞”提前乘二。OpenMV 的
`TARGET_REACHED` 或高频完整扫描的最佳候选确定绝对输入频率；此时立即置位
`absolute_frequency_locked` 并响铃一次。

## 直接设置题目图像

绝对频率确认后，不做“一倍对角线预锁”，而是直接设置最终图像：

| 模式 | DDS 频率 | 固定目标相位 |
|---|---:|---:|
| 左下到右上对角线 | `f_in` | 0° |
| 圆 | `f_in` | -90°（POW 使用 270°） |
| “∞” | `2*f_in` | 广义相位 0° |

该路径调用 `set_dds_tone`，其中会先停止 `PLL_Demo`，防止旧的本地 ADC PLL
继续写 DDS。Task5 不再调用 `start_phase_lock`，因此不会重复应用二倍倍率，也
不会在响铃后跳到异常高频。

## 永久视觉闭环

F407 随后可靠发送 `LOCK_HOLD(0x32)`。OpenMV 进入 `STATE_LOCK_HOLD` 后并非
静态等待，而是持续发送 16 字节 `VISUAL_LOCK_SAMPLE`。F407 使用本地接收时间
驱动 `VisualLock_Step`，避免把两块板的启动时钟误当成同步时钟。

视觉控制参数：

- 搜索中心：题目最终输出频率；
- 硬边界：中心 `±5 Hz`；
- 方向探测：`±0.1 Hz`；
- 最终 DDS 网格：0.01 Hz；
- 命令稳定等待：200 ms；
- 每 4 个有效样本形成一个控制窗口；
- 进入稳定阈值：60 mHz；
- 重捕获阈值：连续 3 个窗口高于 180 mHz；
- 单次最大纠偏：1 Hz。

控制器先比较基线与正负探测结果，再沿正确方向做递减步进。进入稳定阈值后
停止继续跨越最优点，只收集稳定证据，避免半周期往返振荡。进入 LOCKED 后仍
持续处理样本；若源频率漂移则原地重新探测方向和收敛。题目相位在整个 Session
中固定，视觉算法只调频率，不会把圆或“∞”改成对角线。

图像样本暂时消失时不触发错误或退出，只保持最后 DDS 命令。首个视觉样本也可
作为 LOCK_HOLD 已被 OpenMV 接受的证据。若命令/ACK 丢失，快速重试耗尽后每
1 s 继续发送相同幂等命令，直到视觉链路恢复。

## 保持与人工退出

第一次视觉稳定后 Task5 显示状态锁存为 `LOCKED`。内层视觉控制器后续重捕获
不会让任务离开该状态。正常路径没有成功 EXIT、图像超时 EXIT 或通信超时 EXIT。

允许退出的路径只有：

- 串口屏返回、页面切换或显式 `exit`：`EXIT_TASK(reason=1)`；
- 实体模式键重选：`EXIT_TASK(reason=4)` 并开始新 Session；
- 系统复位。

`reason=0` 已废止，两端都不会用它自动结束任务。

## Host regression checks

```powershell
gcc -std=c11 -Wall -Wextra -Werror -IUsr\TASK5 analysis\test_visual_lock.c Usr\TASK5\visual_lock.c -lm -o analysis\test_visual_lock.exe
.\analysis\test_visual_lock.exe

gcc -std=c11 -Wall -Wextra -Werror -IUsr\TASK5 analysis\test_task5_controller.c Usr\TASK5\task5_controller.c Usr\TASK5\openmv_protocol.c Usr\TASK5\visual_lock.c -o analysis\test_task5_controller.exe
.\analysis\test_task5_controller.exe

python analysis\test_openmv_lock_hold_protocol.py
python analysis\test_lissajous_stability.py
```

协议全文见 `OpenMV/Task5与OpenMV串口通信协议-最终版.md`。
