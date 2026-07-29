# AD9910 一倍/二倍锁相 Demo

## 已实现功能

- ADC1/ADC2 双重规则同步采样：
  - CH1：`PC0 / ADC1_IN10`，接信号发生器
  - CH2：`PC1 / ADC2_IN11`，接 AD9910 输出反馈
  - TIM2 TRGO 触发，DMA2 Stream0 循环双半缓冲
  - 12 bit、3 cycles 采样时间，最高 `2.4 MSPS`
- 根据信号发生器输入自动判频，并把 DDS 设置到 `1×` 或 `2×` 频率。
- 使用同一批同步样本计算广义相位：

  ```text
  phase = φDDS - N × φREF，N = 1 或 2
  ```

- 相位误差经环形低通和 PI 环路转成 DDS 频率微调，不清相位累加器，
  避免控制过程人为跳相。
- DDS Profile 0 始终使用 `ASF = 0x3FFF` 满数字幅度（`STOP`/错误安全状态除外）。
- 采样率自动设置为：

  ```text
  Fs = clamp(24 × N × Fref, 10 kHz, 2.4 MHz)
  ```

  信号丢失后回到 2.4 MHz 重新搜索，避免用低采样率搜索时产生混叠误判。

## 数据路径和执行上下文

```text
TIM2 TRGO
    -> ADC1(PC0) + ADC2(PC1) 同时转换
    -> ADC 公共 CDR（低 16 bit=ADC1，高 16 bit=ADC2）
    -> DMA 循环双半缓冲
    -> DMA ISR 只发布 half-ready/full-ready
    -> 主循环：判频 -> I/Q 相位解调 -> PI 锁相 -> 更新 AD9910 Profile 0

USART1 ISR -> RX 字节环形缓冲 -> 主循环解析命令 -> 更新逻辑配置
```

中断中没有浮点分析、字符串格式化、DDS SPI 发送或延时。

## 输入硬件要求

STM32F407 ADC 不能直接接双极性信号。两路输入必须经过限幅/偏置和缓冲：

- 电压范围严格保持在 `0~3.3 V`
- 正弦波中心建议偏置到约 `1.65 V`（ADC 码约 2048）
- 两路前端增益、延时尽量一致
- 3-cycle 采样时间要求较低源阻抗；建议运放缓冲
- 信号发生器、DDS 板和 STM32 必须共地

算法的默认有效幅度门限是约 32 ADC codes 峰峰值，可在
`phase_detector.c` 的 `PHASE_MIN_P2P_COUNTS` 调整。

## USART1 命令

接口为 `PA9/TX`、`PA10/RX`，`115200 8N1`。命令以 CR 或 LF 结束，
不区分大小写。

| 命令 | 作用 |
|---|---|
| `MUL 1` | DDS 锁定为输入基频 |
| `MUL 2` | DDS 锁定为输入二倍频 |
| `PHASE 30` | 设置广义相位目标为 +30° |
| `PHASE -90.5` | 设置广义相位目标为 -90.5° |
| `STATUS` | 输出频率、相位、采样率、状态和错误计数 |
| `STOP` | 停止采样并关闭 DDS 数字幅度 |
| `START` | 重新满幅启动并搜索/锁定 |
| `HELP` | 输出命令帮助 |

二倍频时，`PHASE` 设置的不是两个不同频率正弦波的普通相位差，而是
`φDDS - 2φREF`。该量恒定时，二倍频莉萨如图形会稳定。

## 状态含义

- `SEARCH`：在 2.4 MHz 下寻找有效输入频率。
- `ACQUIRE`：DDS 已接近目标倍频，环路正在收敛相位。
- `LOCKED`：相位误差连续 50 ms 小于约 3°。
- `STOPPED`：串口停止状态。
- `ERROR`：ADC/DDS 启动或更新失败，输出已转为零幅度。

`STATUS` 中 `OVR/ADCERR/RXOVR/DDSERR` 均应长期为 0。若 `OVR` 增长，
说明主循环来不及消费 DMA 半缓冲，应使用 Release 优化或降低最高采样率。

## 频率范围

相位闭环要求 DDS 反馈频率低于采样率的 45%：

```text
N × Fref < 0.45 × Fs
```

在 2.4 MHz 上限下，理论上 1× 输入应低于约 1.08 MHz，2× 输入基频应
低于约 540 kHz。靠近上限时每周期采样点很少，实际可用上限还取决于
模拟前端、DDS 重建滤波器和目标相位抖动；建议从 1~100 kHz 完成首轮上板
标定，再逐步提高。

## 验收步骤

1. 先断开 ADC，确认 AD9910 固定 10 kHz 满幅输出和 USART1 命令正常。
2. 给 PC0/PC1 输入偏置后的同频信号，发送 `MUL 1`、`PHASE 0`，
   确认状态由 `SEARCH -> ACQUIRE -> LOCKED`。
3. 示波器 XY 模式观察 1× 莉萨如图，改变 `PHASE`，图形应稳定旋转后停止。
4. 发送 `MUL 2`，确认 DDS 频率为输入两倍；观察稳定的二倍频莉萨如图。
5. 改变输入频率，确认 `STATUS` 中 `FS` 随 `24 × N × Fref` 调整，
   且四个错误计数不增长。
6. 若稳态仍有可见抖动，先校正两路模拟前端延时/幅度，再微调
   `lock_controller.c` 中的相位滤波时间常数、`Kp` 和 `Ki`。

## 验证命令

纯算法合成正弦测试：

```powershell
gcc -std=c11 -Wall -Wextra -Werror -IUsr\PLL `
  analysis\test_phase_lock.c `
  Usr\PLL\phase_detector.c Usr\PLL\lock_controller.c `
  -lm -o analysis\test_phase_lock.exe
.\analysis\test_phase_lock.exe
```

固件构建：

```powershell
cmake --build build\verify
```
