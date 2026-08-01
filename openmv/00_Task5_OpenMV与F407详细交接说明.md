# Task5 OpenMV 与 F407 交接说明（协议 v2）

详细帧定义见 `Task5与OpenMV串口通信协议-最终版.md`。两端必须成套烧录：

| 端 | 入口文件 | 协议版本 |
|---|---|---:|
| STM32F407 | `Usr/TASK5/openmv_protocol.h`、`task5_controller.c` | `0x02` |
| OpenMV N6 | `OpenMV/OpenMV_main_task5_uart.py` | `0x02` |

## 当前主流程

```text
人工选择题型
  -> OpenMV 等待有效信号输入（二维 XY 轨迹连续 6 帧）
  -> DAC 1 kHz/10 kHz 粗识别
  -> 单倍 DDS 100 Hz 网格确定绝对输入频率
  -> 立即响铃
  -> F407 直接设置题目图像
  -> LOCK_HOLD(session, mode, input_hz, output_hz)
  -> OpenMV 持续发送 VISUAL_LOCK_SAMPLE
  -> F407 在目标输出 ±5 Hz 内微调 DDS，使李萨如图像稳定
  -> 一直保持，直到人工退出或人工重选
```

`START_TASK` 只表示实体按键已经选定题型，不表示外部信号源已经接入。
OpenMV 收到并 ACK 后先进入 `WAIT_SIGNAL`；此时不加载 PCA/KNN 模型、不累计
粗识别历史，也不启动 30 秒粗识别回退计时。无输入参考画面中的绿色细竖线
不算有效信号；只有绿色 XY 轨迹在横、纵两个方向都有足够跨度并连续满足
6 帧，才进入 `COARSE`。任一无效帧都会把连续计数清零。

目标图形：对角线为左下到右上、单倍频、0°；圆为单倍频、-90°；“∞”
为二倍频、广义相位 0°。“∞”只在绝对频率确定后乘二一次。

Task5 主路径不启动本地 ADC PLL。`LOCK_HOLD` 不是静态等待状态：OpenMV 在
该状态持续分析图像并发送样本。F407 锁定后也继续处理样本；漂移时原地重捕获，
但 Task5 的 LOCKED 显示保持锁存。

## 保持与退出边界

- 无人工操作：不发送 EXIT，不回锯齿波，不回 OpenMV IDLE；
- 图像暂时丢失：F407 保持最后 DDS 命令，等待样本恢复；
- LOCK_HOLD ACK/命令丢失：快速重试后每 1 s 继续重发，任务不退出；
- 视觉纠偏：严格限制在目标输出频率 `±5 Hz`；
- 实体模式键重选：允许切换到新 Session；
- 串口屏返回/切换/显式 `exit`：发送非零原因的 EXIT_TASK；
- `reason=0`：OpenMV 必须 NACK，保持状态不变。

正常流程不得出现：

```text
【任务结束】任务N，原因0，已回到空闲
```

只有明确人工退出时才允许打印 `【人工退出】`。

## 联调顺序

1. 核对 UART7/UART5 交叉接线、共地和 `115200 8N1`；
2. 确认双方 `VER=0x02`；
3. 不接外部信号按模式键，确认 OpenMV 一直显示 `WAIT SIGNAL`，不发送
   `COARSE_RESULT`；
4. 接入信号，确认连续 6 帧后才显示 `COARSE` 并开始频率识别；
5. 验证三种模式的 DDS_TEST 始终为单倍候选；
6. TARGET 后确认只响铃一次，并收到 `LOCK_HOLD(0x32)`；
7. 检查对角线方向、圆及“∞”图形正确；
8. 记录 DDS 命令，确认所有视觉微调都未超过目标 `±5 Hz`；
9. 让源频率小幅漂移，确认图像重新稳定且 Task5 不退出；
10. 暂时断开 OpenMV TX，确认 DDS 保持；恢复后确认视觉样本与闭环恢复；
11. 用串口屏 `exit`/返回键退出，确认双方回安全态/IDLE；
12. 在保持态按另一模式键，确认能人工切换到新 Session。

## 主机回归

```powershell
gcc -std=c11 -Wall -Wextra -Werror -IUsr\TASK5 analysis\test_openmv_protocol.c Usr\TASK5\openmv_protocol.c -o analysis\test_openmv_protocol.exe
gcc -std=c11 -Wall -Wextra -Werror -IUsr\TASK5 analysis\test_visual_lock.c Usr\TASK5\visual_lock.c -lm -o analysis\test_visual_lock.exe
gcc -std=c11 -Wall -Wextra -Werror -IUsr\TASK5 analysis\test_task5_controller.c Usr\TASK5\task5_controller.c Usr\TASK5\openmv_protocol.c Usr\TASK5\visual_lock.c -o analysis\test_task5_controller.exe
python analysis\test_openmv_lock_hold_protocol.py
python analysis\test_openmv_input_signal_gate.py
python analysis\test_lissajous_stability.py
```
