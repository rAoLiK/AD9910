# AD9910 F 题系统集成说明

## 1. 集成结果

本目录把题目任务、串口屏输入、板级继电器/按键和原有锁相模块组合为
一个非阻塞应用状态机。USART6 只服务 TJC 串口屏，USART2 只服务
OpenMV；原 `pll_demo.c` 中的 ASCII CLI 已移除。锁相计算与 Task5
协议状态转换只在主循环执行，ADC/DMA、UART 和按键 ISR 只发布固定
大小事件或搬运一个字节。

主循环入口：

```c
while (1) {
  AppIntegration_Process();
}
```

## 2. 文件职责

| 文件 | 职责 |
|---|---|
| `app_core.h/.c` | 纯 C 应用状态机、逻辑命令、错误收敛；不包含 HAL 或 TJC 类型 |
| `app_hmi_map.h/.c` | 页面/控件/触发沿到逻辑命令的表驱动映射 |
| `app_board.h/.c` | PE1 输出通路、PE0 DDS/DAC 选择、PA0/PB9/PB8 按键、EXTI 和安全输出 |
| `app_integration.h/.c` | 装配 PLL、TJC、板级端口，调度主循环并集中转发 UART 回调 |
| `../TASK5/*` | USART2 OpenMV 协议、Task5 状态机及 PA4 DAC 锯齿波 |
| `../PLL/pll_demo.*` | 非阻塞锁相服务及 ADC ISR 入口 |
| `../SCREEN_TJC/*` | USART6 传输、协议解析和控件命令 |

依赖方向：

```text
TJC UART ISR -> RX 字节环 -> 主循环协议解析 -> HMI 映射 -> 逻辑命令队列
按键 EXTI ISR -> 饱和计数器 -> 短按/长按判定 ----------^
USART2 ISR -> RX 字节环 -> OpenMV 帧解析 -> Task5 状态机
主循环 -> app_core -> board/PLL 端口 -> PLL_Demo_Process
ADC DMA ISR -> 半缓冲就绪位 -----------> PLL_Demo_Process
app_core 状态 -> HMI 语义渲染 -> 非阻塞 UART TX 环
```

## 3. 应用状态

| 状态 | 输出路径 | DDS/ADC | Task5 EXTI |
|---|---|---|---|
| `MENU_SAFE` | 直通安全位 | DDS 零幅度、ADC 停止 | 禁用 |
| `TASK14` + `DIRECT` | 直通 | DDS 零幅度、ADC 停止 | 禁用 |
| `TASK14` + `LOCK_*` | DDS | 搜索/捕获/锁定 | 禁用 |
| `TASK5` 等待选择/粗识别 | 直通安全位 | PA4 DAC 输出锯齿波 | 启用 |
| `TASK5` DDS 搜索 | DDS | 相机判频，ADC 停止 | 启用 |
| `TASK5` 视觉锁定 | DDS | OpenMV 视觉反馈，ADC/PLL 停止 | 启用 |
| `TASK5` + `ERROR` | DAC | 保持所选满码域锯齿波，显示错误原因 | 启用 |
| `ERROR_SAFE` | 直通安全位 | DDS 零幅度、ADC 停止 | 禁用 |

所有状态转换先回安全基线，再进入目标状态。屏幕启动或唤醒只触发当前
状态重绘，不会重复启动业务；页面 ID 上报只作为确认接收，不回发切页
命令，避免形成页面确认回环。

## 4. 页面和控件映射

仅处理按下事件 `0x65 ... 0x01 FF FF FF`。应用状态改变后由 MCU 主动
发送 `page <id>` 命令；屏幕按钮自身不负责切页。

| 页面 | component ID | 逻辑命令 |
|---:|---:|---|
| 0 | 2 | 进入 Task1-4 页面安全空闲态 |
| 0 | 3 | 进入 Task5，等待实体按键选择 |
| 1 | 2 | 返回菜单安全态 |
| 1 | 3 | Task2：1×、实际输出 +90° |
| 1 | 4 | Task3：2×、实际广义相位 0° |
| 1 | 6/7/8/9 | 设置 2/4/6/8 div |
| 1 | 10 | Task1 直通 |
| 2 | 2 | 返回菜单安全态 |

Task4 保持最近选择的图形类型：

- 最近是 Task2/Task3：锁相环继续运行，只更新 ASF，不重新判频。
- 最近是 Task1：选择 2/4/6 div 时切到 DDS 1×同相锁定并用 ASF 调幅；
  选择 8 div 时停止 PLL 并切回物理直通，不使用 DDS 输出。
- 最近类型跨菜单安全态保留；再次从 page0 b1 进入 Task1-4 后，直接按
  幅度键时，Task1 的 8 div 直接恢复物理直通，其余组合按该类型启动锁相。
  上电后尚无最近类型时仍拒绝幅度命令。
- 尚未选择 Task1/2/3 时按幅度键会被拒绝并累计
  `rejected_command_count`。

## 5. 相位约定

`phase_detector` 输出：

```text
phase_feedback = phi_ADC2 - multiplier * phi_ADC1
```

ADC2 反馈与真实 Y 输出固定反相 180°。当前选择：

| 图形 | multiplier | 真实输出目标 | 反馈锁相目标 |
|---|---:|---:|---:|
| Task1 的 2/4/6 div DDS 调幅 | 1 | 0° | 180° |
| Task2 圆 | 1 | +90° | -90° |
| Task3 ∞ | 2 | 0°广义相位 | 180° |

可调宏位于 `app_integration.c`：

```c
APP_FEEDBACK_INVERSION_DEG
APP_TASK1_OUTPUT_PHASE_DEG
APP_TASK2_OUTPUT_PHASE_DEG
APP_TASK3_GENERALIZED_PHASE_DEG
```

若示波器上的圆旋向或“∞”方向需要改变，只调整这些宏，不修改
`phase_detector` 或 `lock_controller`。

## 6. 幅度换算

当前标定：

```text
示波器：0.5 V/div
ASF 最大、放大后：约 4.2 Vpp
scale = target_div * 0.5 / 4.2
```

对应初始比例约为：

| Y 幅度 | 目标 Vpp | ASF 比例 |
|---:|---:|---:|
| 2 div | 1.0 V | 0.2381 |
| 4 div | 2.0 V | 0.4762 |
| 6 div | 3.0 V | 0.7143 |
| 8 div | 4.0 V | 0.9524 |

上板标定时只修改：

```c
APP_SCOPE_VOLTS_PER_DIV
APP_DDS_FULL_SCALE_VPP_AFTER_GAIN
```

ADC2 在放大器前，因此它只用于频率/相位反馈，不能验证放大后绝对幅度；
最终 0.2 div 误差必须以示波器 Y 轴实测校准。

## 7. 板级资源

| 资源 | 配置 |
|---|---|
| PE1 | 输出通路选择；默认高电平直通，低电平选择 DDS 路径 |
| PE0 | Task5 DDS/DAC 选择；高电平选择 DDS/Task1–4 支路，低电平选择 PA4 DAC 锯齿波 |
| PA0 | Task5 对角线（左下到右上的正斜率），按下接 3.3 V，内部下拉，EXTI 上升沿 |
| PB9 | Task5 圆，按下接地，内部上拉，EXTI 下降沿 |
| PB8 | Task5 ∞，按下接地，内部上拉，EXTI 下降沿 |
| USART6 PC6/PC7 | TJC，115200 8N1，逐字节 RX/TX 中断 |
| USART2 PA2/PA3 | OpenMV TX/RX，115200 8N1，逐字节中断和环形缓冲 |
| PA4 | DAC_OUT1，Task5 单调递增锯齿波 |
| TIM6/DMA1 Stream5 | 100 点锯齿波触发与循环搬运 |
| DMA2 Stream0 | ADC1/ADC2 双重模式采样，保持原分配 |

PE1 输出通路极性由 `APP_RELAY_DIRECT_ACTIVE_HIGH` 控制。PE0 极性固定为
高电平选择 DDS/Task1–4 支路、低电平选择 DAC 支路：Task1–4 和安全初始
状态保持高电平；Task5 启动锯齿波前拉低，粗识别结束且 DDS 配置成功后
重新拉高。Task5 通信或运行错误时重新拉低并保持本次选择的 DAC 锯齿波，
只有用户退出 Task5 后才恢复高电平。三个按键 IRQ 仅在 Task1–4 状态下
关闭；主循环只在 Task5 控制器有效时接受按键，并另做 50 ms 去抖。
同一模式键短按（按住时间小于 400 ms）选择 1 kHz，长按（按住时间
大于等于 400 ms）选择 10 kHz。按压时长判定仅在后台进行，屏幕不显示
短按、长按、等待判定状态或任何 DAC 锯齿波频率；判定完成后直接进入
相应模式。Task5 已经通信、
搜索、锁相、锁定或进入错误态后仍可再次选择：STM32 依次排队发送旧
Session 的 `EXIT_TASK(reason=0x04)` 和新 Session 的 `START_TASK`，然后
进入新模式。Task5 待选择页面固定分三行显示
`PA0: diagonal`、`PB9: circle`、`PB8: infinity`。

Task5 的 DDS_TEST 始终先用单倍频率确定绝对输入频率。确认后立即响铃并发送
协议 v2 的 `LOCK_HOLD`，不再先锁一倍频对角线。STM32 直接设置题目图像并
停止本地 PLL；OpenMV 随后持续发送图像运动样本，STM32 只在目标输出频率
`±5 Hz` 内微调 DDS。锁定后视觉反馈仍持续，图像或通信暂时中断只保持最后
DDS 命令，不会自动退出。实体模式键重选、串口屏返回/切换和显式 `exit`
仍是允许的人工退出路径，并发送非零原因的 `EXIT_TASK`。

## 8. TJC 非阻塞规则

- RX ISR 把一个字节写入 512 字节环形缓冲并立即重启接收。
- `TJC_Service()` 在主循环解析完整帧并调用 HMI adapter。
- TX API 只把完整命令原子地放入 2048 字节 TX 环，立即返回。
- TX 完成 ISR 只推进一个字节并启动下一个字节。
- ISR 中调用 TJC 发送 API 会返回 `HAL_BUSY` 并累计
  `isr_send_reject_count`。
- 原控件库中的同步 `get`/透传等待函数仍为兼容 API，应用状态机不调用。

`TJC_GetDiagnostics()` 可读取 RX 溢出/重启、TX 溢出/错误和 ISR 误调用
计数。

## 9. PLL 服务接口

```c
HAL_StatusTypeDef PLL_Demo_Init(ad9910_t *dds);
HAL_StatusTypeDef PLL_Demo_Configure(uint8_t multiplier,
                                     float target_phase_deg,
                                     float output_scale);
HAL_StatusTypeDef PLL_Demo_Start(void);
HAL_StatusTypeDef PLL_Demo_Stop(void);
void PLL_Demo_Process(void);
const pll_demo_status_t *PLL_Demo_GetStatus(void);
```

`Init` 只初始化并保持输出关闭；`Configure` 不接触 HMI。仅改变幅度时不会
重置判频或锁相；改变倍率时回到 2.4 MSPS 搜索。`Start` 和 `Stop` 由应用
状态机显式调用。

原有以下保护保持不变：

- 2.4 MSPS alias-safe 搜索。
- 动态采样率 `clamp(24*N*Fref, 10 kHz, 2.4 MHz)`。
- `CHANGE_PENDING` 独立 600 ms 绝对硬超时。
- 一倍/二倍变化和稳定门限按输入频率域定义。
- DMA ISR 不执行浮点、DDS SPI、日志或状态转换。

## 10. 构建和测试

主机测试：

```powershell
gcc -std=c11 -Wall -Wextra -Werror -IUsr\PLL `
  analysis\test_phase_lock.c `
  Usr\PLL\phase_detector.c Usr\PLL\lock_controller.c `
  -lm -o build\host-tests\test_phase_lock.exe

gcc -std=c11 -Wall -Wextra -Werror -IUsr\APP `
  analysis\test_app_core.c `
  Usr\APP\app_core.c Usr\APP\app_hmi_map.c `
  -o build\host-tests\test_app_core.exe
```

目标构建：

```powershell
cmake --build build\cube-release
```

当前 Release 构建：

```text
FLASH: 44,096 bytes
RAM:   27,168 bytes
```

## 11. 上板验收清单

1. 上电停留 page0，PE1 为高并选择直通，DDS 零幅度，ADC/TIM2 未运行。
2. page0 b1 进入 page1，再由 page1 b7 进入 Task1，直通对角线正确。
3. page1 b1 在 1-100 kHz 任意输入下进入 1×锁定并显示圆。
4. page1 b2 进入 2×锁定并显示上下左右对称的“∞”。
5. Task1 后选择 2/4/6 div 时确认走 DDS 且幅度误差不超过 0.2 div；
   选择 8 div 时确认 PLL 停止、PE1 切到物理直通。Task2/3 的
   2/4/6/8 div 均保持 DDS 锁相。
6. 切换输入频率，确认重新捕获且没有长期停留
   `frequency_change_pending`。
7. page2 中 PA0/PB9/PB8 分别选择直线/圆/∞；短按输出 1 kHz 锯齿波，
   长按至少 400 ms 输出 10 kHz。进入任一 Task5 运行/锁定/错误状态后，
   再次短按或长按另一个模式键，确认旧 Session 被停止并进入新模式；
   Task1–4 页面按键无作用。
8. 断开 USART2 或让 OpenMV 不应答，确认仍停留在 Task5 页面，屏幕显示
   具体错误以及 `RX/CRC/U` 计数，PE0 为低且所选满码域 DAC 锯齿波持续。
9. 绝对频率确认后检查 DDS 直接呈现所选图像，所有视觉纠偏均在目标输出
   `±5 Hz` 内；锁定后让源频率小幅漂移，确认能原地重捕获且不退出。
10. 暂时断开 OpenMV TX，确认 DDS 保持；恢复通信后确认视觉闭环恢复。
11. 用户退出 Task5 后回到直通安全态，DAC、DDS 和 ADC 停止。
12. 检查 TJC 与 OpenMV 的 RX/TX/CRC/队列诊断计数不异常增长。

Task5 已实现 USART2 帧解析、CRC、ACK/NACK、同 SEQ 重发、Session/Test
去重、粗识别、DDS 测试搜索以及命中后向本地相位锁定的交接。OpenMV
端必须按 `ref/Task5与OpenMV串口通信协议-最终版.md` 实现对应状态机。
声光提示不在本工程现有硬件范围内。

## 12. 本次集成验证记录

2026-07-30 已完成：

- `analysis/test_phase_lock.c` 全部通过，覆盖 1×/2×、分频段检测、切频
  重捕获、倍率一致的输入域变化门限和低频偏差场景。
- `analysis/test_app_core.c` 全部通过，覆盖 PRESS 映射、Task1 直通、
  Task1 的 2 div 切换 DDS 及 8 div 返回物理直通、Task2 的 8 div
  仍保持 DDS、Task5 按键入口和错误锁存。
- STM32CubeProgrammer 2.23.0 通过 SWD 将 `AD9910.elf` 写入
  STM32F42xxx/F43xxx，下载校验成功并完成软件复位；连接电压 3.25 V。
- 板上经应用命令队列执行 Task1 `2 div -> 8 div`：前者为
  `LOCKED`/DDS/采样运行，后者为 `DIRECT`/物理直通/PLL 停止/采样停止。

2026-07-31 Task5 调试验证：

- 三项主机测试 `test_openmv_protocol`、`test_task5_controller` 和
  `test_app_core` 全部通过；Task5 测试覆盖 ACK 超时、首次发送失败、
  ACK 重发阶段发送失败以及错误态保持 DAC。
- `cmake --build build/cube-release` 成功；资源占用为 FLASH 44,104
  bytes、RAM 27,168 bytes。
- CubeProgrammer 2.23.0 通过 SWD 下载、校验并复位成功；目标为
  STM32F42xxx/F43xxx，Device ID `0x419`，连接电压 3.24 V。
- 旧固件现场捕获到 `START_TASK ACK timeout`：ACK 已重发 3 次，
  OpenMV 合法帧、CRC 错误、UART 硬件错误和 TX 队列错误均为 0。
- 新固件主动发送的 9 字节 HEARTBEAT 已全部离开 USART2 TX 环并完成
  发送中断，但 OpenMV 未回 ACK；故障边界位于 STM32 UART5_TX 之后、
  OpenMV 回传之前。
- 新固件板上复现无 ACK 后保持 `TASK5 + ERROR`；实测 PE0 为低，
  DAC1/TIM6/DMA1 Stream5 均运行，锯齿表端点为 `0x000/0xFFF`，
  DAC 启动错误和 DMA 欠载计数均为 0。

本轮未确认 COM12 的 CH340 是否实际连接到 TJC，因此未从 PC 注入 TJC
触摸帧，也不能复用已经移除的 ASCII CLI 读取锁相状态。第 11 节中需要
串口屏、信号源和示波器参与的项目仍属于现场验收项；在完成这些实测前，
不得把倍率/相位/幅度的本轮硬件回归标记为通过。
