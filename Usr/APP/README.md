# AD9910 F 题系统集成说明

## 1. 集成结果

本目录把题目任务、串口屏输入、板级继电器/按键和原有锁相模块组合为
一个非阻塞应用状态机。USART1 只服务 TJC 串口屏；原 `pll_demo.c` 中的
ASCII CLI 已移除。锁相计算仍然只在主循环执行，ADC/DMA、UART 和按键
ISR 只发布固定大小事件或搬运一个字节。

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
| `app_board.h/.c` | PE5 继电器、PA0/PB9/PB8 按键、EXTI 和安全输出 |
| `app_integration.h/.c` | 装配 PLL、TJC、板级端口，调度主循环并集中转发 UART 回调 |
| `../PLL/pll_demo.*` | 非阻塞锁相服务及 ADC ISR 入口 |
| `../SCREEN_TJC/*` | USART1 传输、协议解析和控件命令 |

依赖方向：

```text
TJC UART ISR -> RX 字节环 -> 主循环协议解析 -> HMI 映射 -> 逻辑命令队列
按键 EXTI ISR -> 事件位 -------------------------------^
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
| `TASK5_PLACEHOLDER` | 直通安全位 | DDS 零幅度、ADC 停止 | 启用 |
| `ERROR_SAFE` | 直通安全位 | DDS 零幅度、ADC 停止 | 禁用 |

所有状态转换先回安全基线，再进入目标状态。屏幕启动或唤醒只触发当前
状态重绘，不会重复启动业务；页面 ID 上报只作为确认接收，不回发切页
命令，避免形成页面确认回环。

## 4. 页面和控件映射

仅处理按下事件 `0x65 ... 0x01 FF FF FF`。应用状态改变后由 MCU 主动
发送 `page <id>` 命令；屏幕按钮自身不负责切页。

| 页面 | component ID | 逻辑命令 |
|---:|---:|---|
| 0 | 3 | 进入 Task1-4 页面安全空闲态 |
| 0 | 4 | 进入 Task5 占位状态 |
| 1 | 2 | 返回菜单安全态 |
| 1 | 3 | Task2：1×、实际输出 +90° |
| 1 | 4 | Task3：2×、实际广义相位 0° |
| 1 | 6/7/8/9 | 设置 2/4/6/8 div |
| 1 | 10 | Task1 直通 |
| 2 | 2 | 返回菜单安全态 |

Task4 保持最近选择的图形类型：

- 最近是 Task2/Task3：锁相环继续运行，只更新 ASF，不重新判频。
- 最近是 Task1：物理直通无法调幅，因此切到 DDS 1×同相锁定，再按 ASF
  生成相同的对角线图形。
- 最近类型跨菜单安全态保留；再次从 page0 b1 进入 Task1-4 后，直接按
  幅度键会按该类型重新启动锁相。上电后尚无最近类型时仍拒绝幅度命令。
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
| Task1 DDS 调幅 | 1 | 0° | 180° |
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
| PE5 | 继电器；默认高电平直通 |
| PA0 | Task5 对角线，按下接 3.3 V，内部下拉，EXTI 上升沿 |
| PB9 | Task5 圆，按下接地，内部上拉，EXTI 下降沿 |
| PB8 | Task5 ∞，按下接地，内部上拉，EXTI 下降沿 |
| USART1 PA9/PA10 | TJC，115200 8N1，逐字节 RX/TX 中断 |
| DMA2 Stream0 | ADC1/ADC2 双重模式采样，保持原分配 |

继电器极性由 `APP_RELAY_DIRECT_ACTIVE_HIGH` 控制。三个按键 IRQ 仅在
`TASK5_PLACEHOLDER` 中启用，主循环另做 50 ms 去抖。

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

集成时的 Release 基线：

```text
FLASH: 35,216 bytes
RAM:   24,984 bytes
```

## 11. 上板验收清单

1. 上电停留 page0，PE5 为直通，DDS 零幅度，ADC/TIM2 未运行。
2. page0 b1 进入 page1，再由 page1 b7 进入 Task1，直通对角线正确。
3. page1 b1 在 1-100 kHz 任意输入下进入 1×锁定并显示圆。
4. page1 b2 进入 2×锁定并显示上下左右对称的“∞”。
5. Task1/2/3 后依次选择 2/4/6/8 div，实测误差不超过 0.2 div。
6. 切换输入频率，确认重新捕获且没有长期停留
   `frequency_change_pending`。
7. page2 中 PA0/PB9/PB8 更新 `t2` 占位状态；其他页面按键无作用。
8. 任意页面退出后回到直通安全态，DDS 和 ADC 停止。
9. 检查 PLL 的 `OVR/ADCERR/DDSERR` 以及 TJC 的 RX/TX/队列诊断计数
   不增长。

Task5 摄像头识别、声光提示和 10 s/5 s 自动流程尚未实现；当前仅保留
三个模式的稳定逻辑入口、按键资源和屏幕状态显示。

## 12. 本次集成验证记录

2026-07-29 已完成：

- `analysis/test_phase_lock.c` 全部通过，覆盖 1×/2×、分频段检测、切频
  重捕获、倍率一致的输入域变化门限和低频偏差场景。
- `analysis/test_app_core.c` 全部通过，覆盖 PRESS 映射、Task1 直通、
  Task1 调幅切换 DDS、Task2 调幅不重启、Task5 按键占位和错误安全收敛。
- `cmake --build build/cube-release` 成功；最终资源占用为 FLASH 35,216
  bytes、RAM 24,984 bytes。
- STM32CubeProgrammer 2.23.0 通过 SWD 将 `AD9910.elf` 写入
  STM32F42xxx/F43xxx，下载校验成功并完成软件复位；连接电压 3.24 V。

当前主机没有枚举原自动板测使用的 COM10，因此不能从 PC 注入 TJC
触摸帧，也不能复用已经移除的 ASCII CLI 读取锁相状态。第 11 节中需要
串口屏、信号源和示波器参与的项目仍属于现场验收项；在完成这些实测前，
不得把倍率/相位/幅度的本轮硬件回归标记为通过。
