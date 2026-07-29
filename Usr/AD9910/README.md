# AD9910 驱动库使用说明书

## 1. 驱动库概述

本驱动库面向 **STM32F407ZGT6 + STM32 HAL** 平台，用于控制 **AD9910 直接数字频率合成器（DDS）**。  
驱动库支持 AD9910 的常用控制功能，包括：

1. 设备初始化与硬件复位
2. 串行寄存器读写
3. 固定频率正弦输出
4. 频率、相位、幅度独立配置
5. Profile 快速切换
6. 数字斜坡发生器（DRG）控制
7. 输出移键控（OSK）控制
8. RAM 波形装载与任意波形输出
9. 软件 SPI / 硬件 SPI / 硬件 SPI + DMA 三种通信方式

本驱动库的设计目标是：

- **对新手友好**：提供完整配置说明和示例代码
- **对工程友好**：全部引脚使用宏或结构体配置，便于移植
- **对性能友好**：控制引脚统一使用 `GPIOx->BSRR` 进行高速操作
- **对扩展友好**：接口覆盖单音、扫频、任意波形等典型应用

---

## 2. 文件组成

```text
.
├── ad9910.h      // 驱动头文件：宏配置、数据结构、函数声明
├── ad9910.c      // 驱动源文件：具体实现
└── README.md     // 使用说明书
```

---

## 3. 驱动依赖

本驱动依赖如下组件：

1. STM32 HAL 库
2. `stm32f4xx_hal.h`
3. 至少一组可用 GPIO
4. 任选其一：
   - 软件 SPI 所需 GPIO
   - 硬件 SPI 外设
   - 硬件 SPI + DMA

---

## 4. 外设与 IO 说明

下表说明驱动可能使用到的外设和 IO，以及它们的用途、是否必须配置。

### 4.1 外设说明

| 外设/资源 | 作用 | 是否必须 | 说明 |
|---|---|---:|---|
| GPIO | 控制 AD9910 各控制脚 | 是 | 所有模式都需要 |
| 软件 SPI GPIO | 用于位操作模拟 SPI | 否 | 默认使用该模式 |
| 硬件 SPI | 用于高速串行通信 | 否 | 与软件 SPI 二选一 |
| DMA | 用于 SPI DMA 传输 | 否 | 仅硬件 SPI + DMA 时使用 |
| 定时器 | 实现精确定时扫频 | 否 | 指数扫频/定时步进时推荐 |

### 4.2 引脚说明

| 引脚名 | 方向（相对 MCU） | 是否必须 | 作用 | 说明 |
|---|---|---:|---|---|
| `CS` | 输出 | 是 | SPI 片选 | 低电平有效 |
| `RESET` | 输出 | 是 | 芯片硬件复位 | 上电初始化必须使用 |
| `IO_UPDATE` | 输出 | 是 | 将缓冲寄存器内容提交到工作寄存器 | 写寄存器后通常需要触发 |
| `IO_RESET` | 输出 | 否，推荐 | 复位串行接口状态机 | **不会清除功能寄存器**，仅在 SPI 传输异常时恢复接口同步 |
| `PROFILE0` | 输出 | 否 | 选择 Profile bit0 | 需要 Profile 功能时使用 |
| `PROFILE1` | 输出 | 否 | 选择 Profile bit1 | 同上 |
| `PROFILE2` | 输出 | 否 | 选择 Profile bit2 | 同上 |
| `DR_HOLD` | 输出 | 否 | DRG 保持控制 | 使用线性扫频时可选 |
| `DR_CTL` | 输出 | 否 | DRG 方向/控制 | 使用线性扫频时可选 |
| `DR_OVER` | 输入 | 否 | DRG 超量程指示 | 用于检测是否到达边界 |
| `OSK` | 输出 | 否 | 外部幅度移键控控制 | 使用 OSK 时需要 |
| `SOFT_SCLK` | 输出 | 软件 SPI 必须 | 软件 SPI 时钟 | 默认模式使用 |
| `SOFT_SDIO` | 输出 | 软件 SPI 必须 | 软件 SPI 数据输出 | 默认模式使用 |
| `SOFT_SDO` | 输入 | 否 | 软件 SPI 数据输入 | 仅读回时需要 |
| `SPI_SCK` | 复用 | 硬件 SPI 必须 | 硬件 SPI 时钟 | 硬件 SPI 模式使用 |
| `SPI_MOSI` | 复用 | 硬件 SPI 必须 | 硬件 SPI 输出 | 硬件 SPI 模式使用 |
| `SPI_MISO` | 复用 | 否 | 硬件 SPI 输入 | 读回功能时推荐连接 |

---

## 5. 通信模式选择

驱动支持以下三种通信模式：

### 5.1 软件 SPI（默认）

特点：

- 最容易移植
- 不占用硬件 SPI 外设
- 适合新手快速验证功能
- 通信速度低于硬件 SPI

配置宏：

```c
#define AD9910_SPI_MODE AD9910_SPI_MODE_SOFTWARE
```

### 5.2 硬件 SPI

特点：

- 通信速度更高
- CPU 占用更低
- 适合正式工程使用

配置宏：

```c
#define AD9910_SPI_MODE   AD9910_SPI_MODE_HARDWARE
#define AD9910_SPI_HANDLE (&hspi1)
#define AD9910_SPI_USE_DMA 0U
```

### 5.3 硬件 SPI + DMA

特点：

- 适合大批量传输
- 适合较高更新速率场景
- 需要 DMA 配置与中断管理

配置宏：

```c
#define AD9910_SPI_MODE    AD9910_SPI_MODE_HARDWARE
#define AD9910_SPI_HANDLE  (&hspi1)
#define AD9910_SPI_USE_DMA 1U
```

---

## 6. 零基础快速上手

如果你是第一次使用该驱动，建议按如下顺序操作：

1. 先选择 **软件 SPI 模式**
2. 先实现 **固定频率正弦输出**
3. 再尝试 **指数扫频**
4. 最后再尝试 **线性扫频** 和 **任意波形输出**

推荐最小可运行配置：

- `CS`
- `RESET`
- `IO_UPDATE`
- `SOFT_SCLK`
- `SOFT_SDIO`

如果只做“固定频率输出”，以上引脚即可。

---

## 7. CubeMX 配置步骤

## 7.1 软件 SPI 模式配置步骤

### 第一步：GPIO 配置

在 CubeMX 中将以下引脚配置为 `GPIO_Output`：

- `CS`
- `RESET`
- `IO_UPDATE`
- `SOFT_SCLK`
- `SOFT_SDIO`

推荐附加配置：

- `IO_RESET`
- `PROFILE0/1/2`
- `DR_HOLD`
- `DR_CTL`
- `OSK`

若使用 `DR_OVER` 或 `SOFT_SDO`，请配置为 `GPIO_Input`。

### 第二步：输出参数建议

- 模式：推挽输出
- 上拉/下拉：默认无上下拉即可
- 速度：建议 `High` 或 `Very High`

### 第三步：代码宏配置

```c
#define AD9910_SPI_MODE        AD9910_SPI_MODE_SOFTWARE
#define AD9910_SOFT_SCLK_PORT  GPIOA
#define AD9910_SOFT_SCLK_PIN   GPIO_PIN_5
#define AD9910_SOFT_SDIO_PORT  GPIOA
#define AD9910_SOFT_SDIO_PIN   GPIO_PIN_7
```

---

## 7.2 硬件 SPI 模式配置步骤

### 第一步：启用 SPI 外设

在 CubeMX 中启用 `SPI1`（或其它 SPI），推荐配置：

- Mode: `Full Duplex Master`
- Data Size: `8 Bits`
- First Bit: `MSB First`
- NSS: `Software`

### 第二步：配置 SPI 相关引脚

- `SCK`：复用为 SPI 时钟
- `MOSI`：复用为 SPI 数据输出
- `MISO`：如需读回则配置

### 第三步：控制引脚保持 GPIO

以下引脚依然使用普通 GPIO，不要交给 SPI 外设：

- `CS`
- `RESET`
- `IO_UPDATE`
- `IO_RESET`
- `PROFILE0/1/2`
- `DR_HOLD`
- `DR_CTL`
- `OSK`

### 第四步：宏配置

```c
#define AD9910_SPI_MODE      AD9910_SPI_MODE_HARDWARE
#define AD9910_SPI_HANDLE    (&hspi1)
#define AD9910_SPI_USE_DMA   0U
```

---

## 7.3 硬件 SPI + DMA 模式配置步骤

在硬件 SPI 模式基础上继续：

1. 在 SPI 对应外设页中添加 DMA 发送通道
2. 若使用读回，则添加 DMA 接收通道
3. 打开 DMA 中断
4. 检查 NVIC 优先级

宏配置：

```c
#define AD9910_SPI_MODE      AD9910_SPI_MODE_HARDWARE
#define AD9910_SPI_HANDLE    (&hspi1)
#define AD9910_SPI_USE_DMA   1U
```

> 注意：DMA 模式下，SPI 发送/接收完成的时序管理应由上层工程配合处理中断或状态机。

---

## 8. 基础初始化示例

```c
#include "ad9910.h"

static ad9910_t g_ad9910;

void DDS_Init(void)
{
  ad9910_init_t init = AD9910_INIT_DEFAULT();

  /* 示例：25 MHz 参考时钟，40 倍频 → 1 GHz SYSCLK */
  init.sysclk.ref_clk_hz = 25000000ULL;
  init.sysclk.sys_clk_hz = 1000000000ULL;
  init.sysclk.pll_enable = 1U;
  init.sysclk.pll_multiplier = 40U;
  init.sysclk.vco_sel = AD9910_VCO_SEL_5;

  (void)AD9910_Init(&g_ad9910, &init);
}
```

若使用 40 MHz 参考时钟：

```c
  init.sysclk.ref_clk_hz = 40000000ULL;
  init.sysclk.sys_clk_hz = 1000000000ULL;
  init.sysclk.pll_enable = 1U;
  init.sysclk.pll_multiplier = 25U;      /* 40 MHz × 25 = 1 GHz */
  init.sysclk.vco_sel = AD9910_VCO_SEL_5;
  init.sysclk.icp = 1U;                  /* 根据环路滤波器调整 */
```

---

## 9. 使用案例

## 9.1 输出固定频率正弦（最基础功能）

```c
void DDS_Output_10MHz(void)
{
  /* 输出 10 MHz，0°，满幅 */
  (void)AD9910_SetSingleToneHz(&g_ad9910, 10000000.0, 0.0, 1.0, 1U);
}
```

说明：

- 这是最基础、最推荐的验证方式
- 若该示例工作正常，再进行扫频和 RAM 波形测试

---

## 9.2 实现指数增加扫频（频率改变时相位不变）

```c
#include <math.h>

void DDS_ExpSweep_PhaseContinuous(double f_start_hz, double f_stop_hz,
                                  uint32_t points, uint32_t step_delay_ms) {
  uint32_t i;
  double ratio;
  double f_now;
  ad9910_status_t status;

  if ((points < 2U) || (f_start_hz <= 0.0) || (f_stop_hz <= f_start_hz)) {
    return;
  }

  ratio = pow(f_stop_hz / f_start_hz, 1.0 / (double)(points - 1U));
  f_now = f_start_hz;

  /* 预先计算并固定 POW/ASF（保持相位连续）*/
  uint16_t pow_word = AD9910_PhaseDegToPOW(0.0);
  uint16_t asf_word = AD9910_AmplitudeScaleToASF(0.8);

  /* 把 Profile0 初始化为起始点（一次性写入，确保 ASF 非 0）*/
  ad9910_profile_word_t prof;
  prof.ftw =
      AD9910_FrequencyToFTW(f_now, (double)g_ad9910.cfg.sysclk.sys_clk_hz);
  prof.pow = pow_word;
  prof.asf = asf_word;
  status = AD9910_ProgramProfile(&g_ad9910, AD9910_PROFILE_0, &prof, 1U);
  if (status != AD9910_STATUS_OK) {
    Error_Handler();
    return;
  }

  /* 循环只更新 Profile0 的 FTW（保持 POW/ASF 不变）*/
  for (i = 0U; i < points; ++i) {
    prof.ftw =
        AD9910_FrequencyToFTW(f_now, (double)g_ad9910.cfg.sysclk.sys_clk_hz);
    status = AD9910_ProgramProfile(&g_ad9910, AD9910_PROFILE_0, &prof, 1U);
    if (status != AD9910_STATUS_OK) {
      Error_Handler();
      return;
    }
    HAL_Delay(step_delay_ms);
    f_now *= ratio;
  }
}
```

关键点：

1. 扫频过程中只改 `FTW`
2. 不修改 `POW`
3. 不触发清相位累加器相关位

这样可以保证改频时相位连续，不产生人为相位跳变。

---

## 9.3 实现线性扫频（使用 DDS 自带功能）

```c
void DDS_LinearSweep(void)
{
  ad9910_drg_config_t drg = {
    .destination = AD9910_DRG_DEST_FREQUENCY,
    .lower_limit = AD9910_FrequencyToFTW(1000000.0,  (double)g_ad9910.cfg.sysclk.sys_clk_hz),
    .upper_limit = AD9910_FrequencyToFTW(10000000.0, (double)g_ad9910.cfg.sysclk.sys_clk_hz),
    .step_up = AD9910_FrequencyToFTW(1000.0, (double)g_ad9910.cfg.sysclk.sys_clk_hz),
    .step_down = AD9910_FrequencyToFTW(1000.0, (double)g_ad9910.cfg.sysclk.sys_clk_hz),
    .rate_up = 10,
    .rate_down = 10,
    .no_dwell_low = 0,
    .no_dwell_high = 0,
    .enable = 1
  };

  (void)AD9910_ConfigureDRG(&g_ad9910, &drg, 0U);
  (void)AD9910_EnableDRG(&g_ad9910, 1U, 1U);
}
```

说明：

- 该方法使用 AD9910 自带 DRG 功能
- `DR_CTL` 和 `DR_HOLD` 可用于进一步控制扫描过程
- 更适合硬件自动线性扫频场景

---

## 9.4 实现任意波形输出

### 方式一：使用内置常用波形

```c
void DDS_Output_Triangle(void)
{
  ad9910_waveform_config_t cfg = {
    .type = AD9910_WAVE_TRIANGLE,
    .min_code = 0,
    .max_code = 16383,
    .duty_cycle = 0.5,
    .sinc_cycles = 4.0
  };

  ad9910_ram_profile_config_t ram_profile = {
    .step_rate = 1,
    .start_addr = 0,
    .end_addr = 1023,
    .mode = AD9910_RAM_MODE_CONT_RECIRCULATE,
    .no_dwell_high = 0,
    .zero_crossing = 0
  };

  (void)AD9910_ConfigureRamMode(&g_ad9910, 1U, AD9910_RAM_DEST_AMPLITUDE, 0U);
  (void)AD9910_LoadBuiltInWaveform(&g_ad9910, &cfg, 0U);
  (void)AD9910_ProgramRamProfile(&g_ad9910, AD9910_PROFILE_0, &ram_profile, 0U);
  (void)AD9910_SelectProfilePins(&g_ad9910, AD9910_PROFILE_0);
  (void)AD9910_IOUpdate(&g_ad9910);
}
```

### 方式二：装载自定义 1024 点波形

```c
static uint16_t user_wave[AD9910_RAM_POINT_COUNT];

void DDS_Output_CustomWave(void)
{
  uint16_t i;

  for (i = 0U; i < AD9910_RAM_POINT_COUNT; ++i)
  {
    /* 示例：自定义 0~16383 波形码 */
    user_wave[i] = (uint16_t)((i * 16383U) / 1023U);
  }

  (void)AD9910_ConfigureRamMode(&g_ad9910, 1U, AD9910_RAM_DEST_AMPLITUDE, 0U);
  (void)AD9910_LoadRamWaveform(&g_ad9910, user_wave, 1U);
}
```

说明：

- RAM 点数固定 1024 点
- 每点有效范围 `0~16383`
- 若只是常见波形，优先用 `AD9910_LoadBuiltInWaveform`，因为内置表放在 Flash，不占 MCU RAM

---

## 10. 驱动库全部函数说明

下面按功能分类给出所有主要接口的用途说明。

## 10.1 基础控制函数

| 函数 | 作用 |
|---|---|
| `AD9910_Init` | 初始化设备对象并写入基础配置 |
| `AD9910_DeInit` | 清空设备对象 |
| `AD9910_Reset` | 触发芯片硬件复位 |
| `AD9910_IOUpdate` | 提交寄存器缓冲内容 |
| `AD9910_IOReset` | 复位串口状态机 |

## 10.2 寄存器访问函数

| 函数 | 作用 |
|---|---|
| `AD9910_WriteRegister` | 写指定寄存器 |
| `AD9910_ReadRegister` | 读指定寄存器 |

## 10.3 单音控制函数

| 函数 | 作用 |
|---|---|
| `AD9910_SetFTW` | 设置频率调谐字 |
| `AD9910_SetPOW` | 设置相位偏移字 |
| `AD9910_SetASF` | 设置幅度比例字 |
| `AD9910_SetSingleTone` | 一次性设置 FTW/POW/ASF |
| `AD9910_SetSingleToneHz` | 以 Hz/度/归一化幅度配置单音 |
| `AD9910_GetSingleTone` | 读取当前单音参数 |

## 10.4 Profile 控制函数

| 函数 | 作用 |
|---|---|
| `AD9910_ProgramProfile` | 写指定 Profile |
| `AD9910_ReadProfile` | 读指定 Profile |
| `AD9910_SelectProfilePins` | 驱动 PROFILE[2:0] 引脚 |

## 10.5 DRG / OSK / 扩展控制函数

| 函数 | 作用 |
|---|---|
| `AD9910_ConfigureDRG` | 配置数字斜坡发生器 |
| `AD9910_EnableDRG` | 使能或关闭 DRG |
| `AD9910_SetDRHold` | 设置 DR_HOLD 电平 |
| `AD9910_SetDRCtl` | 设置 DR_CTL 电平 |
| `AD9910_GetDROver` | 读取 DR_OVER 电平 |
| `AD9910_SetOSKPin` | 设置 OSK 电平 |
| `AD9910_ConfigureManualOSK` | 使能外部手动 OSK 控制 |

## 10.6 功耗与辅助 DAC 函数

| 函数 | 作用 |
|---|---|
| `AD9910_SetAuxDacFsc` | 设置辅助 DAC 码 |
| `AD9910_GetAuxDacFsc` | 获取辅助 DAC 码 |
| `AD9910_SetPowerDown` | 设置功耗控制位 |
| `AD9910_GetPowerDown` | 获取功耗控制位 |

## 10.7 RAM 与波形函数

| 函数 | 作用 |
|---|---|
| `AD9910_WriteRam` | 连续写入 RAM 32 位数据 |
| `AD9910_ReadRam` | 连续读取 RAM 32 位数据 |
| `AD9910_ProgramRamProfile` | 配置 RAM Profile |
| `AD9910_ReadRamProfile` | 读取 RAM Profile |
| `AD9910_ConfigureRamMode` | 使能 RAM 并设置播放目标 |
| `AD9910_GenerateWaveform` | 生成 1024 点波形数据 |
| `AD9910_LoadRamWaveform` | 将波形码表装载到 AD9910 RAM |
| `AD9910_LoadBuiltInWaveform` | 一键装载三角波/方波/SINC |

## 10.8 数据换算函数

| 函数 | 作用 |
|---|---|
| `AD9910_FrequencyToFTW` | 频率转 FTW |
| `AD9910_FTWToFrequency` | FTW 转频率 |
| `AD9910_PhaseDegToPOW` | 相位角转 POW |
| `AD9910_POWToPhaseDeg` | POW 转相位角 |
| `AD9910_AmplitudeScaleToASF` | 归一化幅度转 ASF |
| `AD9910_ASFToAmplitudeScale` | ASF 转归一化幅度 |

---

## 11. 使用注意事项

## 11.1 写寄存器后通常需要 IO_UPDATE

很多 AD9910 寄存器写入后不会立即生效，必须通过 `AD9910_IOUpdate()` 提交。

## 11.2 IO_RESET 不是芯片复位

`IO_RESET` 只复位串口状态机，不会清除功能寄存器，不等价于 `RESET`。

## 11.3 固定频率功能是第一验证步骤

建议任何新工程先做“固定频率正弦输出”，确认 SPI、RESET、IO_UPDATE、时钟配置均正确后，再做扫频和 RAM 波形。

## 11.4 RAM 波形输出的关键条件

必须同时满足：

1. RAM 已使能
2. RAM 目标已正确选择
3. RAM 数据已写入
4. RAM Profile 已配置
5. 已选择 Profile 并 `IO_UPDATE`

若任一项缺失，RAM 波形通常不会正常输出。

## 11.5 读回功能的硬件要求

若要使用读回，必须满足：

- 软件 SPI 时已连接 `SOFT_SDO`
- 硬件 SPI 时已连接 MISO/SDO

否则读回接口会返回不支持或无法得到正确结果。

## 11.6 DMA 模式的使用限制

DMA 模式下，底层驱动仅发起 DMA 传输；若你的应用需要严格同步，应在工程中配合 DMA 完成中断进行状态管理。

---

## 12. 常见问题与解决建议

### 12.1 编译报错：`unknown type name 'SPI_HandleTypeDef'`

请检查：

1. `HAL_SPI_MODULE_ENABLED` 是否开启
2. `main.h` 是否先于 `ad9910.h` 包含
3. `spi.c` 是否已加入工程构建

### 12.2 软件 SPI 没有波形输出

请检查：

1. `AD9910_SPI_MODE` 是否为软件 SPI
2. `SOFT_SCLK / SOFT_SDIO` 宏是否与实际引脚一致
3. GPIO 是否配置为推挽输出

### 12.3 改频后波形相位跳变

若你希望相位连续，请：

1. 只更新 `FTW`
2. 不重写 `POW`
3. 不清相位累加器

### 12.4 DR_OVER 一直没有变化

请检查：

1. `DR_OVER` 是否配置为输入
2. `DRG` 是否真的使能
3. 扫描是否运行到了边界值

### 12.5 RAM 波形不正确

请检查：

1. 数据点数是否为 1024
2. 每点范围是否在 `0~16383`
3. 是否正确选择了 `AD9910_RAM_DEST_AMPLITUDE`
4. 是否正确配置了 RAM Profile

### 12.6 初始化后功耗上升但无波形输出

请检查：

1. **参考时钟与倍频系数是否匹配硬件**：例如硬件使用 40 MHz 晶振，则 `ref_clk_hz` 应设为 40000000，`pll_multiplier` 应设为 25（40×25=1GHz）。若错误使用默认值（25MHz×40），PLL 将尝试锁定到 1.6 GHz（超出范围），导致无输出。
2. **VCO 选择是否正确**：若启用了 PLL（`pll_enable = 1`），必须设置 `vco_sel` 为有效 VCO 频段（`AD9910_VCO_SEL_0` ~ `AD9910_VCO_SEL_5`），不能使用默认的 `AD9910_VCO_SEL_PLL_BYPASS_7`（该值为 PLL 旁路模式）。例如，1 GHz SYSCLK 应使用 `AD9910_VCO_SEL_5`。
3. **PLL 倍频系数范围**：`pll_multiplier` 必须在 12~127 之间（AD9910 数据手册规定）。
4. **PLL 锁定时间**：驱动在初始化末尾已添加约 1 ms 的 PLL 锁定等待，但若你的环路滤波器参数与推荐值差异较大，可能需要更长时间。可通过 `PLL_LOCK` 引脚确认锁定状态。
5. **DAC 输出硬件连接**：AD9910 的 DAC 输出为电流模式（开漏），IOUT 引脚需通过 50 Ω 电阻连接到 AGND 才能形成电压输出。

---

## 13. 建议使用顺序

推荐按以下顺序逐步验证：

1. 初始化驱动
2. 固定频率正弦输出
3. 指数扫频
4. 线性扫频（DRG）
5. 任意波形（RAM）
