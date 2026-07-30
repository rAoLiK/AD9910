# AD9910 数字锁相工程说明

## 1. 当前锁相逻辑结论

本工程是**分频段混合数字锁相环**，不能简单称为“全程 PI 锁相”：

- 中频/高频捕获阶段使用 PI 频率环。相位误差经环形低通后，
  `Kp` 产生即时频率校正，`Ki` 积分到 `frequency_trim_hz`。
- PI 环还叠加了相位变化率估计，用于补偿频率偏差并增加阻尼；它不是
  独立的完整 PID。
- 低频段主要使用 AD9910 的 POW（Phase Offset Word）直接校相，并用较弱
  的积分和相位变化率学习修正 FTW。低频不是与中高频相同的 PI 环。
- 输入达到约 59 kHz、确认锁定并稳定后，进入高频保持态：冻结为单一
  整数 FTW，关闭相邻 FTW 的分数调制，用小步 POW 调整剩余相位漂移。

相位检测量为：

```text
generalized_phase = phi_ADC2 - N * phi_ADC1
phase_error       = wrap(generalized_phase - target_phase)
N                 = 1 或 2
```

中高频 PI 主环可概括为：

```text
trim[k+1] = clamp(trim[k] - Ki * filtered_error * dt)

desired_frequency =
    coarse_frequency
    + trim
    - Kp * filtered_error
    - phase_rate_gain * estimated_frequency_error

command_frequency += clamp(desired_frequency - command_frequency,
                           maximum_step)
```

负号来自本工程的误差定义。不要只根据常见 PI 公式的正负号直接改代码。

## 2. 数据路径和执行上下文

```text
TIM2 TRGO
  -> ADC1/PC0（参考输入）+ ADC2/PC1（DDS 反馈）同步采样
  -> ADC 公共 CDR
  -> DMA2 Stream0 循环双半缓冲
  -> ADC DMA ISR 只发布 half/full-ready
  -> 主循环 PhaseDetector_Process
  -> 主循环 LockController_Step
  -> 主循环 PLL_Demo_ServiceDDS
  -> AD9910 Profile 0（FTW + POW + ASF）

TJC UART ISR -> 字节环 -> 主循环协议解析 -> HMI 映射
             -> app_command 队列 -> app_core -> PLL_Demo_Start/Stop
```

上电默认状态是 `MENU_SAFE`，不是闭环状态。必须先进入 `TASK14`，再选择：

- Task2：1×、实际输出目标 +90°，反馈目标 -90°。
- Task3：2×、实际广义相位目标 0°，反馈目标 180°。

中断中不执行浮点锁相、DDS SPI 写入、字符串格式化或阻塞等待。

## 3. 60 kHz 以上临稳抽动的代码原因与修正

### 3.1 可形成周期性大跳的机制

旧逻辑在 `ACQUIRE` 持续 1 s 后无条件执行：

```text
清空频率/相位控制器
-> 回到 2.4 MSPS
-> 重新判频
-> 重新捕获
```

高频相位估计噪声较大。如果原始误差偶尔越过“连续 50 ms 小于 3°”的锁定
判据，环路即使已经接近稳定也可能在 1 s watchdog 到期时被整体重启。
外部表现正是“快稳定时突然大幅抽动，然后重复捕获”。

另一个高频抖动源是分数 FTW 调制。1 GHz DDS 时钟下一个 FTW LSB 约为
0.233 Hz；普通捕获态会在相邻 FTW 间切换以获得更好的平均频率分辨率，
但该切换会直接表现为瞬时频率/相位抖动。

### 3.2 本次高频专用修正

- 高频、频率和相位均有效、滤波相位误差已进入 30° 近锁区时，不再让
  1 s acquire watchdog 清空整个环路，只延长本次捕获窗口。
- `CHANGE_PENDING` 的 600 ms 超时仍具有最高优先级，真实输入变频仍会
  重搜，不会被近锁保护掩盖。
- 固定 FTW 保持态入口从 68 kHz 下移到 59 kHz，退出阈值为 57 kHz，
  从而可靠覆盖标称 60 kHz 及以上输入并保留迟滞。
- 高频保持态 POW 增益由 0.25 调为 0.35，单次上限由 0.06° 调为 0.08°，
  以覆盖 60 kHz 输入、2× 输出时的最坏 FTW 量化残差。
- 新增锁丢失、保持态进入/退出、近锁重启抑制计数，便于板上判断是否
  仍在发生肉眼不易捕捉的状态往返。

低频 `LOCK_BAND_LOW` 的 PI/POW 参数、进入/退出阈值、采样策略和 3 s
watchdog 均未修改。新增 watchdog 保护还显式要求参考频率不低于
`PLL_HIGH_HOLD_ENTER_HZ`，不会进入低频路径。

## 4. 锁相参数、位置和作用

### 4.1 采样率和高频边界

位置：[Usr/PLL/pll_config.h](Usr/PLL/pll_config.h)

| 参数 | 当前值 | 含义 | 增大后的主要影响 |
|---|---:|---|---|
| `PLL_MIN_SAMPLE_RATE_HZ` | 10 kHz | 动态采样率下限 | 低频块时间变短、CPU/噪声带宽增加 |
| `PLL_MAX_SAMPLE_RATE_HZ` | 2.4 MHz | ADC 采样率上限 | 需要重新确认 ADC/TIM/DMA 时序 |
| `PLL_TIM2_CLOCK_HZ` | 72 MHz | TIM2 输入时钟 | 必须与实际时钟树一致，不能用于调环 |
| `PLL_SAMPLES_PER_DDS_CYCLE` | 24 | 期望每个 DDS 周期样本数 | 相位估计更细、采样率和 CPU 占用更高 |
| `PLL_HIGH_HOLD_ENTER_HZ` | 59 kHz | 固定 FTW 保持态允许进入的参考频率 | 更多频率继续使用 PI/分数 FTW |
| `PLL_HIGH_HOLD_EXIT_HZ` | 57 kHz | 保持态退出阈值 | 迟滞减小，边界附近更易反复切换 |

`ENTER` 必须大于 `EXIT`。若要求覆盖标称 60 kHz，应给频率估计误差留出
余量，不建议把 `ENTER` 精确设为 60000。

### 4.2 PI、直接相位和锁定判据

位置：[Usr/PLL/lock_controller.c](Usr/PLL/lock_controller.c)

`s_band_parameters` 每行依次为：

```text
phase_filter_tau_s
frequency_track_tau_s
Kp = proportional_hz_per_rad
Ki = integral_hz_per_rad_s
input_change_hold_s
mid_step_hz
coarse_step_hz
```

| 频段 | 进入/退出规则 | 相位滤波 τ | 判频跟踪 τ | Kp | Ki | 变频稳定时间 | 中/粗步进 |
|---|---|---:|---:|---:|---:|---:|---:|
| LOW | 初始 <40 kHz；<35 kHz 进入，>45 kHz 退出 | 10 ms | 15 ms | 0 | 5 | 60 ms | 0.10 / 0.50 Hz |
| MID | LOW/HIGH 之间 | 4 ms | 4 ms | 20 | 120 | 60 ms | 0.50 / 5 Hz |
| HIGH | 初始 ≥80 kHz；>90 kHz 进入，<75 kHz 退出 | 4 ms | 4 ms | 20 | 120 | 60 ms | 0.50 / 5 Hz |

MID 和 HIGH 当前 PI 数值相同，分频段仍保留，便于后续独立标定。59 kHz
固定 FTW 保持阈值独立于上述控制频段，因此 60 kHz 捕获时仍使用 MID PI，
锁定后进入固定 FTW 保持。

| 参数 | 当前值 | 含义和调节方向 |
|---|---:|---|
| `LOCK_FINE_PHASE_RAD` | 4° | 进入细调的相位误差门限；增大可更早限速，但收敛变慢 |
| `LOCK_MID_PHASE_RAD` | 15° | 中步进/粗步进分界；增大可减少粗调动作 |
| `LOCK_FINE_GRADIENT_HZ` | 0.50 Hz | 进入细调允许的频率梯度 |
| `LOCK_FINE_STEP_HZ` | 0.05 Hz/次 | PI 捕获态细调最大 FTW 频率步进 |
| `LOCK_ACQUIRE_THRESHOLD_RAD` | 3° | 宣告锁定的原始相位误差门限 |
| `LOCK_ACQUIRE_TIME_S` | 50 ms | 必须连续满足锁定条件的时间 |
| `LOCK_RELEASE_THRESHOLD_RAD` | 12° | 掉锁相位误差门限 |
| `LOCK_RELEASE_TIME_S` | 30 ms | 必须连续越界才释放锁定 |
| `LOCK_PHASE_RATE_FILTER_TAU_S` | 6 ms | 相位变化率低通；增大更稳但频差响应更慢 |
| `LOCK_PHASE_RATE_GAIN` | 0.50 | 相位变化率到频率校正的阻尼增益 |
| `LOCK_PHASE_RATE_LIMIT_HZ` | 500 Hz | 相位变化率估计限幅，防止相位跳变变成大频率指令 |
| `LOCK_MAX_DDS_FREQUENCY_HZ` | 400 MHz | 控制器安全频率上限，不是建议工作上限 |
| `LOCK_FREQUENCY_MISSING_RESET_S` | 100 ms | 判频连续丢失多久后清空环路 |

低频 POW 参数：

| 参数 | 当前值 | 含义和调节方向 |
|---|---:|---|
| `LOCK_DIRECT_PHASE_GAIN` | 0.65 | 低频相位误差到 POW 的比例；大则快，也更易过冲 |
| `LOCK_DIRECT_FINE_STEP_RAD` | 0.15°/次 | 低频细调 POW 限幅 |
| `LOCK_DIRECT_MID_STEP_RAD` | 1°/次 | 低频中调 POW 限幅 |
| `LOCK_DIRECT_COARSE_STEP_RAD` | 10°/次 | 低频粗调 POW 限幅 |
| `LOCK_DIRECT_LOCK_STEP_RAD` | 0.10°/次 | 宣告锁定时允许的最大 POW 动作 |
| `LOCK_LOW_RATE_FILTER_TAU_S` | 20 ms | 低频相位变化率滤波 |
| `LOCK_LOW_RATE_LEARN_TAU_S` | 20 ms | 低频由 POW 漂移学习 FTW 偏差的时间常数 |

高频固定 FTW 保持参数：

| 参数 | 当前值 | 含义和调节方向 |
|---|---:|---|
| `LOCK_HIGH_HOLD_SETTLE_S` | 150 ms | 连续锁定条件累计到该值后允许进入保持 |
| `LOCK_HIGH_HOLD_FILTER_TAU_S` | 20 ms | 保持态相位低通；增大可降噪但 POW 跟随更慢 |
| `LOCK_HIGH_HOLD_PHASE_DEADBAND_RAD` | 0.20° | POW 不动作死区；增大抖动更小、静态误差更大 |
| `LOCK_HIGH_HOLD_PHASE_GAIN` | 0.35 | 保持态 POW 比例增益 |
| `LOCK_HIGH_HOLD_PHASE_STEP_RAD` | 0.08°/次 | 保持态 POW 最大单步 |

输入变频识别参数：

| 参数 | 当前值 | 含义和调节方向 |
|---|---:|---|
| `LOCK_INPUT_CHANGE_MIN_HZ` | 40 Hz | 输入变频判据绝对下限 |
| `LOCK_INPUT_CHANGE_FRACTION` | 0.2% | 输入变频判据相对值 |
| `LOCK_INPUT_STABLE_MIN_HZ` | 2 Hz | 新频率稳定窗口绝对下限 |
| `LOCK_INPUT_STABLE_FRACTION` | 0.002% | 新频率稳定窗口相对值 |
| `LOCK_LOW_INPUT_STABLE_HZ` | 1 Hz | 低频稳定窗口上限 |

变频门限实际取 `max(绝对下限, 相对值)`，并在参考输入频率域计算，再按
1×/2× 倍率换算到 DDS 域。

### 4.3 相位/频率检测参数

位置：[Usr/PLL/phase_detector.c](Usr/PLL/phase_detector.c)

| 参数 | 当前值 | 含义和调节方向 |
|---|---:|---|
| `PHASE_MIN_P2P_COUNTS` | 32 codes | 两路信号最小峰峰值；过低会把噪声当信号 |
| `PHASE_MAX_NYQUIST_FRACTION` | 0.45 | DDS 反馈允许占采样率的最大比例 |
| `PHASE_SIGNAL_HOLD_SECONDS` | 120 ms | 无有效过零后仍保留信号的时间 |
| `PHASE_DEMOD_INTERVAL_SECONDS` | 0.75 ms | 最短相位解调间隔 |
| `PHASE_ANALYSIS_POINTS_PER_CYCLE` | 6 | 低频判频目标分析点数/周期 |
| `PHASE_ANALYSIS_MAX_STRIDE` | 8 | 判频抽取最大步长 |
| `PHASE_LOW/MID/HIGH_DEMOD_PAIRS` | 256/128/128 | 各频段 DFT 窗口基础点数 |
| 判频 IIR 系数 | 0.18 | 位于 `PhaseDetector_EstimateFrequency`；减小更稳但切频更慢 |
| DC 跟踪系数 | 0.02 | 位于 `PhaseDetector_Process`；只用于缓慢修正 ADC1 偏置 |

相位质量低于 0.20、任一路幅度过小或 DDS 反馈接近 Nyquist 上限时，
`phase_valid` 会失败。出现这些情况时应先修硬件/采样，不应先调 PI。

### 4.4 调度、watchdog 和 DDS 更新参数

位置：[Usr/PLL/pll_demo.c](Usr/PLL/pll_demo.c)

| 参数 | 当前值 | 含义和调节方向 |
|---|---:|---|
| `PLL_DMA_TARGET_BLOCK_HZ` | 1000 Hz | 低中采样率的目标控制块频率 |
| `PLL_DMA_PROVEN_HIGH_RATE_HZ` | 960 kHz | 达到该采样率后固定 2048 对/半缓冲 |
| `PLL_RATE_CHANGE_HOLDOFF_MS` | 100 ms | 动态采样率再次切换的最短间隔 |
| `PLL_DDS_UPDATE_INTERVAL_MS` | 1 ms | DDS Profile 最短更新间隔 |
| `PLL_ACQUIRE_RESTART_MS` | 1000 ms | 中高频远离近锁区时的捕获重启时间 |
| `PLL_LOW_ACQUIRE_RESTART_MS` | 3000 ms | 低频捕获重启时间，未修改 |
| `PLL_CHANGE_PENDING_RESTART_MS` | 600 ms | 输入变频候选无法稳定时的硬重搜时间 |
| `PLL_HIGH_NEAR_LOCK_ERROR_DEG` | 30° | 高频近锁保护门限；仅延长 acquire watchdog |
| `PLL_INITIAL_DDS_FREQUENCY_HZ` | 10 kHz | 启动前 DDS 初始频率 |

### 4.5 应用目标相位和幅度

位置：[Usr/APP/app_integration.c](Usr/APP/app_integration.c)

| 参数 | 当前值 | 含义 |
|---|---:|---|
| `APP_FEEDBACK_INVERSION_DEG` | 180° | ADC2 反馈通道相对真实输出的固定反相 |
| `APP_TASK1_OUTPUT_PHASE_DEG` | 0° | Task1 DDS 调幅时真实输出目标 |
| `APP_TASK2_OUTPUT_PHASE_DEG` | 90° | Task2 圆的真实输出目标 |
| `APP_TASK3_GENERALIZED_PHASE_DEG` | 0° | Task3 二倍频广义相位目标 |
| `APP_SCOPE_VOLTS_PER_DIV` | 0.5 V/div | 示波器幅度换算 |
| `APP_DDS_FULL_SCALE_VPP_AFTER_GAIN` | 4.432 Vpp | 满幅输出板上标定值 |

圆旋向、∞ 方向或前端反相错误应在这些目标参数中修正，不要用 PI 参数
掩盖固定相位偏差。

## 5. 推荐调参顺序

每次只改一组参数，并同时记录 1×/2×、锁定时间、稳态峰峰相位误差、
`frequency_step_hz`、`phase_step_deg` 和所有诊断计数。

1. **先确认硬件有效性**
   - ADC1/ADC2 必须为 0~3.3 V、约 1.65 V 中点、低源阻抗并共地。
   - `phase_quality` 建议稳定大于 0.5。
   - `dma_overrun_count/adc_error_count/dds_error_count` 自由运行时不得增长。
2. **确认没有误判变频**
   - 稳态时 `frequency_change_pending` 应为 0，
     `frequency_reanchor_count` 不应增长。
   - 若只因判频抖动误触发，先适度增大 `LOCK_INPUT_CHANGE_MIN_HZ`，
     不要先降低 `Kp/Ki`。
3. **调 Kp**
   - 收敛太慢且无振铃：每次增加约 10%~20%。
   - 临稳往返、相位振铃明显：每次降低约 10%~20%。
4. **调 Ki**
   - 长期存在单向相位漂移或静态频偏：小幅增加。
   - 临稳后低频摆动、积分来回拉扯：降低。
   - 始终保证 `frequency_trim_hz` 限幅不会长期顶住。
5. **调相位滤波**
   - 高频随机抖动大：增大 `phase_filter_tau_s`。
   - 捕获明显变钝：减小；随后重新检查 Kp。
6. **调高频保持态**
   - `frequency_step_hz` 必须为 0。
   - POW 高频来回动作：增大死区或滤波时间常数。
   - POW 长期顶到步进上限、最终掉锁：先小幅增加
     `LOCK_HIGH_HOLD_PHASE_STEP_RAD`，再调整增益。
7. **最后调锁定/释放判据**
   - 不要通过放宽锁定阈值伪装控制器没有收敛。
   - `ACQUIRE` 临稳但不锁时先观察
     `acquire_restart_suppressed_count` 和相位误差分布。

为保持低频效果，调整高频问题时优先只改：

```text
PLL_HIGH_HOLD_ENTER_HZ
PLL_HIGH_HOLD_EXIT_HZ
PLL_HIGH_NEAR_LOCK_ERROR_DEG
LOCK_HIGH_HOLD_*
```

不要同时修改 LOW 行参数和 `LOCK_DIRECT_*`。

## 6. 构建、算法测试和烧录

主机算法回归：

```powershell
gcc -std=c11 -Wall -Wextra -Werror -IUsr\PLL `
  analysis\test_phase_lock.c `
  Usr\PLL\phase_detector.c Usr\PLL\lock_controller.c `
  -lm -o build\host-tests\test_phase_lock.exe

.\build\host-tests\test_phase_lock.exe
```

Release 固件：

```powershell
cmake --build build\cube-release
```

STM32CubeProgrammer：

```powershell
STM32_Programmer_CLI.exe `
  -c port=SWD freq=4000 mode=UR `
  -d build\cube-release\AD9910.elf -v -rst
```

## 7. 串口屏进入锁相状态

TJC 触摸事件格式：

```text
65 <page_id> <component_id> 01 FF FF FF
```

从默认菜单进入 1× Task2：

```text
65 00 02 01 FF FF FF   # page0 component2：进入 TASK14
65 01 03 01 FF FF FF   # page1 component3：选择 Task2 1×
```

从 TASK14 进入 2× Task3：

```text
65 01 04 01 FF FF FF   # page1 component4：选择 Task3 2×
```

只有应用状态已经显示 `APP_STATE_TASK14` 且 activity 经历
`LOCK_SEARCH -> LOCK_ACQUIRE -> LOCKED`，才算闭环已经真正启动。

## 8. 板上验收指标

在每个频点、每个倍率下至少自由运行 30 s；读取调试快照时短暂停核会造成
DMA overrun，必须记录读取前后的计数增量，不能把调试暂停引起的计数误判
为运行故障。

| 指标 | 通过条件 |
|---|---|
| 状态 | 连续保持 `PLL_DEMO_LOCKED` |
| `frequency_hold_mode`（≥60 kHz） | 稳定为 1 |
| `frequency_step_hz`（保持态） | 0 |
| `frequency_change_pending` | 0 |
| `lock_loss_count` | 稳态窗口内不增长 |
| `frequency_hold_exit_count` | 稳态窗口内不增长 |
| `frequency_reanchor_count` | 输入不变时不增长 |
| `search_restart_count` | 输入不变时不增长 |
| ADC/DDS 错误 | 不增长 |
| 示波器 XY 图形 | 无周期性大抽动；相位/幅度满足题目要求 |

低频回归至少覆盖 1、4、10、20、39、41 kHz；高频覆盖 60、70、100 kHz；
每个频点均测试 1× 和 2×。固定 100 kHz 输入只能完成高频板测，不能替代
低频信号源和示波器回归。

## 9. 2026-07-30 验证记录

### 改动前基线

- STM32CubeProgrammer 2.23.0 通过 ST-LINK/SWD 识别
  STM32F42xxx/F43xxx，连接电压 3.25 V；Release ELF 下载、校验和复位成功。
- 输入固定为 100 kHz。默认菜单不启动 PLL；进入 TASK14 后分别启动 Task2
  1× 和 Task3 2×。
- 1×：约 99.995 kHz，保持态开启，相位误差约 0.39°；自由运行 30 s
  状态和重捕获计数未变化。
- 2×：约 199.990 kHz，保持态开启，相位误差约 0.27°；自由运行 30 s
  状态和重捕获计数未变化。
- 主机没有枚举可用 COM 口，因此本次由 SWD 向 `app_core` 注入与 TJC
  映射后相同的逻辑命令。应用状态和完整 PLL 闭环已运行，但这不能标记为
  “TJC 串口电气链路已验证”。

### 改动后

- Release ELF 为 34.70 KiB，STM32CubeProgrammer 下载、校验和软件复位
  成功。
- 100 kHz、1×：参考约 99.994 kHz，DDS 指令约 99.995 kHz，稳态相位
  误差约 0.19°；30 s 内保持 `LOCKED`，`hold_enter=1`、
  `hold_exit=0`、`lock_loss=0`、`search_restart=0`，ADC/DDS 错误为 0。
- 100 kHz、2×：参考约 99.994 kHz，DDS 指令约 199.990 kHz，稳态相位
  误差约 -0.32°；切换后以 5 s 快照为基线继续运行 30 s，
  `hold_exit/lock_loss/search_restart` 均未增长，ADC/DDS 错误为 0。
- SWD 暂停并注入 1×/2× 切换时累计了 1 次 `lock_loss` 和 3 次 DMA
  overrun；之后每次 SWD 停核读取快照又可能增加 1 次 DMA overrun。
  自由运行窗口内没有业务状态往返，不能把停核产生的计数算作控制故障。
- 另从 MCU 通过 USART1 发送了 `click b1,1` 屏幕命令：TX 队列正常排空，
  TX 溢出/错误均为 0，但 `rx_restart_count` 仍为 0，应用保持
  `MENU_SAFE`。因此当前连接没有形成 TJC 回传触摸帧；可能是屏幕连接/
  供电未就绪，或屏幕工程不回传命令触发的 click。串口屏实链路仍需用
  实际屏幕按键或接入 USB 串口后复验，不能以 SWD 命令注入代替。
- 主机算法测试覆盖 60/70/100 kHz 的 1×/2× 固定 FTW 量化残差和相位
  扰动，保持态退出次数均为 0；1 kHz 和 4 kHz 低频锁定时间仍分别约
  0.288 s 和 0.215 s。

低频板测仍需要把信号源从当前固定 100 kHz 切换到上述低频矩阵；在完成
低频信号源和示波器回归前，不把低频硬件验收标记为通过。
