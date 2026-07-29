#ifndef AD9910_H
#define AD9910_H

/**
 * @file ad9910.h
 * @brief AD9910 驱动头文件（STM32F407ZGT6 + HAL）。
 *
 * 版本说明（UPDATE1）：
 * 1) 新增软/硬 SPI 宏配置，默认软件 SPI；
 * 2) 硬件 SPI 支持可选 DMA；
 * 3) 新增 DR_HOLD / DR_OVER / DR_CTL / OSK 引脚配置与控制接口；
 * 4) 补充 IO_RESET 的用途与接口说明。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "stm32f4xx_hal.h"

/* ========================= SPI 模式配置（编译期） ========================= */

#define AD9910_SPI_MODE_SOFTWARE         (0U)
#define AD9910_SPI_MODE_HARDWARE         (1U)

/**
 * @brief SPI 模式选择：默认软件 SPI。
 * @note 软件 SPI 便于快速移植；硬件 SPI 速度更高。
 */
#ifndef AD9910_SPI_MODE
#define AD9910_SPI_MODE                  AD9910_SPI_MODE_SOFTWARE
#endif

/**
 * @brief 仅在硬件 SPI 模式下有效：是否启用 DMA。
 */
#ifndef AD9910_SPI_USE_DMA
#define AD9910_SPI_USE_DMA               (0U)
#endif

/**
 * @brief 软件 SPI 的时钟半周期延时（NOP 循环次数）。
 */
#ifndef AD9910_SOFT_SPI_DELAY_CYCLES
#define AD9910_SOFT_SPI_DELAY_CYCLES     (2U)
#endif

/* ========================= 通用配置宏 ========================= */

#ifndef AD9910_ENABLE_READBACK
#define AD9910_ENABLE_READBACK           (0U)
#endif

#ifndef AD9910_SPI_TIMEOUT_MS
#define AD9910_SPI_TIMEOUT_MS            (100U)
#endif

#ifndef AD9910_REF_CLK_HZ_DEFAULT
#define AD9910_REF_CLK_HZ_DEFAULT        (40000000ULL)
#endif

#ifndef AD9910_SYSCLK_HZ_DEFAULT
#define AD9910_SYSCLK_HZ_DEFAULT         (1000000000ULL)
#endif

/* ----------------- 硬件 SPI 句柄（仅硬件 SPI 模式用） ----------------- */
#ifndef AD9910_SPI_HANDLE
#define AD9910_SPI_HANDLE                ((void *)0)
#endif

/* ----------------- 控制引脚（高速 BSRR 翻转） ----------------- */
#ifndef AD9910_IO_UPDATE_PORT
#define AD9910_IO_UPDATE_PORT            ((GPIO_TypeDef *)GPIOE)
#endif
#ifndef AD9910_IO_UPDATE_PIN
#define AD9910_IO_UPDATE_PIN             (GPIO_PIN_2)
#endif

#ifndef AD9910_RESET_PORT
#define AD9910_RESET_PORT                ((GPIO_TypeDef *)GPIOE)
#endif
#ifndef AD9910_RESET_PIN
#define AD9910_RESET_PIN                 (GPIO_PIN_4)
#endif

#ifndef AD9910_CS_PORT
#define AD9910_CS_PORT                   ((GPIO_TypeDef *)GPIOE)
#endif
#ifndef AD9910_CS_PIN
#define AD9910_CS_PIN                    (GPIO_PIN_1)
#endif

#ifndef AD9910_IO_RESET_PORT
#define AD9910_IO_RESET_PORT             ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_IO_RESET_PIN
#define AD9910_IO_RESET_PIN              (0U)
#endif

// 不使用这三个引脚时一定要接地，否则 AD9910 不工作
#ifndef AD9910_PROFILE0_PORT
#define AD9910_PROFILE0_PORT             ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_PROFILE0_PIN
#define AD9910_PROFILE0_PIN              (0U)
#endif

#ifndef AD9910_PROFILE1_PORT
#define AD9910_PROFILE1_PORT             ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_PROFILE1_PIN
#define AD9910_PROFILE1_PIN              (0U)
#endif

#ifndef AD9910_PROFILE2_PORT
#define AD9910_PROFILE2_PORT             ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_PROFILE2_PIN
#define AD9910_PROFILE2_PIN              (0U)
#endif

/* ----------------- UPDATE1 新增功能引脚 ----------------- */

/* DR_HOLD：数字扫描保持输入（输出脚，由 MCU 驱动） */
#ifndef AD9910_DR_HOLD_PORT
#define AD9910_DR_HOLD_PORT              ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_DR_HOLD_PIN
#define AD9910_DR_HOLD_PIN               (0U)
#endif

/* DR_CTL：数字扫描方向/控制输入（输出脚，由 MCU 驱动） */
#ifndef AD9910_DR_CTL_PORT
#define AD9910_DR_CTL_PORT               ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_DR_CTL_PIN
#define AD9910_DR_CTL_PIN                (0U)
#endif

/* DR_OVER：数字扫描越界状态输出（输入脚，由 MCU 读取） */
#ifndef AD9910_DR_OVER_PORT
#define AD9910_DR_OVER_PORT              ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_DR_OVER_PIN
#define AD9910_DR_OVER_PIN               (0U)
#endif

/* OSK：输出移键控控制引脚（输出脚，由 MCU 驱动） */
#ifndef AD9910_OSK_PORT
#define AD9910_OSK_PORT                  ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_OSK_PIN
#define AD9910_OSK_PIN                   (0U)
#endif

/* ----------------- 软件 SPI 引脚（默认模式） ----------------- */

#ifndef AD9910_SOFT_SCLK_PORT
#define AD9910_SOFT_SCLK_PORT            ((GPIO_TypeDef *)GPIOE)
#endif
#ifndef AD9910_SOFT_SCLK_PIN
#define AD9910_SOFT_SCLK_PIN             (GPIO_PIN_3)
#endif

#ifndef AD9910_SOFT_SDIO_PORT
#define AD9910_SOFT_SDIO_PORT            ((GPIO_TypeDef *)GPIOE)
#endif
#ifndef AD9910_SOFT_SDIO_PIN
#define AD9910_SOFT_SDIO_PIN             (GPIO_PIN_0)
#endif

/* 软 SPI 读回数据脚（可选，不接则读回接口返回 NOT_SUPPORTED） */
#ifndef AD9910_SOFT_SDO_PORT
#define AD9910_SOFT_SDO_PORT             ((GPIO_TypeDef *)0)
#endif
#ifndef AD9910_SOFT_SDO_PIN
#define AD9910_SOFT_SDO_PIN              (0U)
#endif

/* ========================= GPIO BSRR 宏 ========================= */

#define AD9910_GPIO_BSRR_SET(pin_)       ((uint32_t)(pin_))
#define AD9910_GPIO_BSRR_CLR(pin_)       (((uint32_t)(pin_)) << 16U)

/* ========================= 寄存器地址与长度 ========================= */

#define AD9910_REG_CFR1                  (0x00U)
#define AD9910_REG_CFR2                  (0x01U)
#define AD9910_REG_CFR3                  (0x02U)
#define AD9910_REG_AUX_DAC               (0x03U)
#define AD9910_REG_IO_UPDATE_RATE        (0x04U)
#define AD9910_REG_FTW                   (0x07U)
#define AD9910_REG_POW                   (0x08U)
#define AD9910_REG_ASF                   (0x09U)
#define AD9910_REG_MULTICHIP_SYNC        (0x0AU)
#define AD9910_REG_DIGITAL_RAMP_LIMIT    (0x0BU)
#define AD9910_REG_DIGITAL_RAMP_STEP     (0x0CU)
#define AD9910_REG_DIGITAL_RAMP_RATE     (0x0DU)
#define AD9910_REG_PROFILE0              (0x0EU)
#define AD9910_REG_PROFILE7              (0x15U)
#define AD9910_REG_RAM                   (0x16U)

#define AD9910_REG_LEN_CFR1              (4U)
#define AD9910_REG_LEN_CFR2              (4U)
#define AD9910_REG_LEN_CFR3              (4U)
#define AD9910_REG_LEN_AUX_DAC           (4U)
#define AD9910_REG_LEN_IO_UPDATE_RATE    (4U)
#define AD9910_REG_LEN_FTW               (4U)
#define AD9910_REG_LEN_POW               (2U)
#define AD9910_REG_LEN_ASF               (4U)
#define AD9910_REG_LEN_MULTICHIP_SYNC    (4U)
#define AD9910_REG_LEN_DIGITAL_RAMP_LIMIT (8U)
#define AD9910_REG_LEN_DIGITAL_RAMP_STEP (8U)
#define AD9910_REG_LEN_DIGITAL_RAMP_RATE (4U)
#define AD9910_REG_LEN_PROFILE           (8U)
#define AD9910_REG_LEN_RAM_WORD          (4U)

#define AD9910_SPI_INSTR_READ_MASK       (0x80U)
#define AD9910_SPI_INSTR_WRITE_MASK      (0x00U)
#define AD9910_SPI_MAKE_INSTR(addr_, is_read_) \
    ((uint8_t)((((is_read_) != 0U) ? AD9910_SPI_INSTR_READ_MASK : AD9910_SPI_INSTR_WRITE_MASK) | ((addr_) & 0x1FU)))

/* ========================= 常量/位定义 ========================= */

#define AD9910_FTW_BITS                  (32U)
#define AD9910_POW_BITS                  (16U)
#define AD9910_ASF_BITS                  (14U)

#define AD9910_FTW_MAX                   (0xFFFFFFFFUL)
#define AD9910_POW_MAX                   (0xFFFFU)
#define AD9910_ASF_MAX                   (0x3FFFU)

#define AD9910_RAM_DEPTH_WORDS           (1024U)
#define AD9910_RAM_ADDR_MAX              (1023U)

#define AD9910_IO_UPDATE_MIN_SYNC_CLK_CYCLES    (1U)
#define AD9910_MASTER_RESET_MIN_SYSCLK_CYCLES   (5U)

#define AD9910_BIT_U32(bit_)                     (1UL << (bit_))

#define AD9910_CFR1_MANUAL_OSK_EXT_CTRL_BIT      (23U)
#define AD9910_CFR1_SELECT_DDS_SINE_BIT          (16U)
#define AD9910_CFR1_OSK_ENABLE_BIT               (9U)
#define AD9910_CFR1_OSK_AUTO_BIT                 (8U)
#define AD9910_CFR1_DIGITAL_POWER_DOWN_BIT       (7U)
#define AD9910_CFR1_DAC_POWER_DOWN_BIT           (6U)
#define AD9910_CFR1_REFCLK_POWER_DOWN_BIT        (5U)
#define AD9910_CFR1_AUX_DAC_POWER_DOWN_BIT       (4U)
#define AD9910_CFR1_EXT_POWER_DOWN_CTRL_BIT      (3U)

#define AD9910_CFR2_AMP_FROM_PROFILE_ENABLE_BIT  (24U)
#define AD9910_CFR2_SYNC_CLK_ENABLE_BIT          (22U)
#define AD9910_CFR2_DRG_DEST_SHIFT               (20U)
#define AD9910_CFR2_DRG_DEST_MASK                (0x3UL << AD9910_CFR2_DRG_DEST_SHIFT)
#define AD9910_CFR2_DRG_ENABLE_BIT               (19U)
#define AD9910_CFR2_DRG_NO_DWELL_HIGH_BIT        (18U)
#define AD9910_CFR2_DRG_NO_DWELL_LOW_BIT         (17U)
#define AD9910_CFR2_MATCHED_LATENCY_BIT          (7U)

#define AD9910_CFR3_VCO_SEL_SHIFT                (24U)
#define AD9910_CFR3_VCO_SEL_MASK                 (0x7UL << AD9910_CFR3_VCO_SEL_SHIFT)
#define AD9910_CFR3_ICP_SHIFT                    (19U)
#define AD9910_CFR3_ICP_MASK                     (0x7UL << AD9910_CFR3_ICP_SHIFT)
#define AD9910_CFR3_REFCLK_DIV_BYPASS_BIT        (15U)
#define AD9910_CFR3_REFCLK_DIV_RESETB_BIT        (14U)
#define AD9910_CFR3_PFD_RESET_BIT                (10U)
#define AD9910_CFR3_PLL_ENABLE_BIT               (8U)
#define AD9910_CFR3_PLL_N_SHIFT                  (1U)
#define AD9910_CFR3_PLL_N_MASK                   (0x7FUL << AD9910_CFR3_PLL_N_SHIFT)

#define AD9910_CFR1_DEFAULT_VALUE                (0x00000000UL)
#define AD9910_CFR2_DEFAULT_VALUE                (0x00400820UL)
#define AD9910_CFR3_DEFAULT_VALUE                (0x1F3F4000UL)

#define AD9910_RAM_POINT_COUNT                   (1024U)

#define AD9910_PROFILE_PACK_SINGLE_TONE(ftw_, pow_, asf_) \
    ((((uint64_t)((asf_) & AD9910_ASF_MAX)) << 48U) | (((uint64_t)(pow_)) << 32U) | ((uint64_t)(ftw_)))

/* ========================= 类型定义 ========================= */

typedef enum
{
  AD9910_STATUS_OK = 0,
  AD9910_STATUS_ERROR,
  AD9910_STATUS_TIMEOUT,
  AD9910_STATUS_INVALID_PARAM,
  AD9910_STATUS_NOT_INITIALIZED,
  AD9910_STATUS_NOT_SUPPORTED,
  AD9910_STATUS_VERIFY_FAILED
} ad9910_status_t;

typedef enum
{
  AD9910_PROFILE_0 = 0,
  AD9910_PROFILE_1,
  AD9910_PROFILE_2,
  AD9910_PROFILE_3,
  AD9910_PROFILE_4,
  AD9910_PROFILE_5,
  AD9910_PROFILE_6,
  AD9910_PROFILE_7
} ad9910_profile_t;

typedef enum
{
  AD9910_DRG_DEST_FREQUENCY = 0,
  AD9910_DRG_DEST_PHASE     = 1,
  AD9910_DRG_DEST_AMPLITUDE = 2
} ad9910_drg_destination_t;

/**
 * @brief AD9910 RAM 播放目标选择。
 */
typedef enum
{
  AD9910_RAM_DEST_FREQUENCY = 0,
  AD9910_RAM_DEST_PHASE     = 1,
  AD9910_RAM_DEST_AMPLITUDE = 2,
  AD9910_RAM_DEST_POLAR     = 3
} ad9910_ram_destination_t;

/**
 * @brief 常用波形类型定义。
 */
typedef enum
{
  AD9910_WAVE_TRIANGLE = 0,
  AD9910_WAVE_SQUARE,
  AD9910_WAVE_SINC
} ad9910_waveform_type_t;

typedef enum
{
  AD9910_RAM_MODE_DIRECT_SWITCH           = 0,
  AD9910_RAM_MODE_RAMP_UP                 = 1,
  AD9910_RAM_MODE_BIDIRECTIONAL_RAMP      = 2,
  AD9910_RAM_MODE_CONT_BIDIRECTIONAL      = 3,
  AD9910_RAM_MODE_CONT_RECIRCULATE        = 4,
  AD9910_RAM_MODE_DIRECT_SWITCH_5         = 5,
  AD9910_RAM_MODE_DIRECT_SWITCH_6         = 6,
  AD9910_RAM_MODE_DIRECT_SWITCH_7         = 7
} ad9910_ram_mode_t;

typedef enum
{
  AD9910_VCO_SEL_0 = 0,
  AD9910_VCO_SEL_1,
  AD9910_VCO_SEL_2,
  AD9910_VCO_SEL_3,
  AD9910_VCO_SEL_4,
  AD9910_VCO_SEL_5,
  AD9910_VCO_SEL_PLL_BYPASS_6,
  AD9910_VCO_SEL_PLL_BYPASS_7
} ad9910_vco_sel_t;

typedef struct
{
  GPIO_TypeDef *port;
  uint16_t pin;
} ad9910_gpio_t;

typedef struct
{
  uint64_t ref_clk_hz;
  uint64_t sys_clk_hz;
  uint8_t pll_enable;
  uint8_t pll_multiplier;
  uint8_t refclk_div_bypass;
  uint8_t refclk_div_resetb;
  ad9910_vco_sel_t vco_sel;
  uint8_t icp;
} ad9910_sysclk_config_t;

typedef struct
{
  uint32_t ftw;
  uint16_t pow;
  uint16_t asf;
} ad9910_single_tone_t;

typedef ad9910_single_tone_t ad9910_profile_word_t;

typedef struct
{
  uint8_t digital_power_down;
  uint8_t dac_power_down;
  uint8_t refclk_input_power_down;
  uint8_t aux_dac_power_down;
  uint8_t ext_power_down_ctrl;
} ad9910_power_down_config_t;

typedef struct
{
  ad9910_drg_destination_t destination;
  uint32_t lower_limit;
  uint32_t upper_limit;
  uint32_t step_up;
  uint32_t step_down;
  uint16_t rate_up;
  uint16_t rate_down;
  uint8_t no_dwell_low;
  uint8_t no_dwell_high;
  uint8_t enable;
} ad9910_drg_config_t;

typedef struct
{
  uint16_t step_rate;
  uint16_t start_addr;
  uint16_t end_addr;
  ad9910_ram_mode_t mode;
  uint8_t no_dwell_high;
  uint8_t zero_crossing;
} ad9910_ram_profile_config_t;

/**
 * @brief 波形生成参数。
 */
typedef struct
{
  ad9910_waveform_type_t type;
  uint16_t min_code;
  uint16_t max_code;
  double duty_cycle;
  double sinc_cycles;
} ad9910_waveform_config_t;

typedef struct
{
  void *hspi; /* 仅在 AD9910_SPI_MODE_HARDWARE 时使用，需指向 SPI_HandleTypeDef */
  uint32_t spi_timeout_ms;
  uint8_t enable_readback;

  ad9910_gpio_t io_update;
  ad9910_gpio_t reset;
  ad9910_gpio_t cs;
  ad9910_gpio_t io_reset;
  ad9910_gpio_t profile0;
  ad9910_gpio_t profile1;
  ad9910_gpio_t profile2;

  /* UPDATE1 新增功能引脚 */
  ad9910_gpio_t dr_hold;
  ad9910_gpio_t dr_ctl;
  ad9910_gpio_t dr_over;
  ad9910_gpio_t osk;

  /* 软件 SPI 引脚 */
  ad9910_gpio_t soft_sclk;
  ad9910_gpio_t soft_sdio;
  ad9910_gpio_t soft_sdo;

  ad9910_sysclk_config_t sysclk;
} ad9910_init_t;

typedef struct
{
  ad9910_init_t cfg;
  uint8_t initialized;
  uint32_t cfr1_shadow;
  uint32_t cfr2_shadow;
  uint32_t cfr3_shadow;
  uint8_t aux_dac_fsc;
} ad9910_t;

#define AD9910_INIT_DEFAULT() \
  { \
    .hspi = AD9910_SPI_HANDLE, \
    .spi_timeout_ms = AD9910_SPI_TIMEOUT_MS, \
    .enable_readback = (uint8_t)AD9910_ENABLE_READBACK, \
    .io_update = { AD9910_IO_UPDATE_PORT, (uint16_t)AD9910_IO_UPDATE_PIN }, \
    .reset = { AD9910_RESET_PORT, (uint16_t)AD9910_RESET_PIN }, \
    .cs = { AD9910_CS_PORT, (uint16_t)AD9910_CS_PIN }, \
    .io_reset = { AD9910_IO_RESET_PORT, (uint16_t)AD9910_IO_RESET_PIN }, \
    .profile0 = { AD9910_PROFILE0_PORT, (uint16_t)AD9910_PROFILE0_PIN }, \
    .profile1 = { AD9910_PROFILE1_PORT, (uint16_t)AD9910_PROFILE1_PIN }, \
    .profile2 = { AD9910_PROFILE2_PORT, (uint16_t)AD9910_PROFILE2_PIN }, \
    .dr_hold = { AD9910_DR_HOLD_PORT, (uint16_t)AD9910_DR_HOLD_PIN }, \
    .dr_ctl = { AD9910_DR_CTL_PORT, (uint16_t)AD9910_DR_CTL_PIN }, \
    .dr_over = { AD9910_DR_OVER_PORT, (uint16_t)AD9910_DR_OVER_PIN }, \
    .osk = { AD9910_OSK_PORT, (uint16_t)AD9910_OSK_PIN }, \
    .soft_sclk = { AD9910_SOFT_SCLK_PORT, (uint16_t)AD9910_SOFT_SCLK_PIN }, \
    .soft_sdio = { AD9910_SOFT_SDIO_PORT, (uint16_t)AD9910_SOFT_SDIO_PIN }, \
    .soft_sdo = { AD9910_SOFT_SDO_PORT, (uint16_t)AD9910_SOFT_SDO_PIN }, \
    .sysclk = { \
      .ref_clk_hz = AD9910_REF_CLK_HZ_DEFAULT, \
      .sys_clk_hz = AD9910_SYSCLK_HZ_DEFAULT, \
      .pll_enable = 0U, \
      .pll_multiplier = 0U, \
      .refclk_div_bypass = 0U, \
      .refclk_div_resetb = 1U, \
      .vco_sel = AD9910_VCO_SEL_PLL_BYPASS_7, \
      .icp = 7U \
    } \
  }

/* ========================= API 声明 ========================= */

/**
 * @brief 初始化 AD9910 设备对象并完成基础寄存器配置。
 * @param dev 设备对象指针。
 * @param init 初始化参数指针。
 * @return 执行状态，成功返回 AD9910_STATUS_OK。
 */
ad9910_status_t AD9910_Init(ad9910_t *dev, const ad9910_init_t *init);

/**
 * @brief 反初始化设备对象，清空运行状态。
 * @param dev 设备对象指针。
 * @return 执行状态，成功返回 AD9910_STATUS_OK。
 */
ad9910_status_t AD9910_DeInit(ad9910_t *dev);

/**
 * @brief 触发 AD9910 主复位引脚脉冲。
 * @param dev 设备对象指针。
 * @return 执行状态，未初始化时返回 AD9910_STATUS_NOT_INITIALIZED。
 */
ad9910_status_t AD9910_Reset(ad9910_t *dev);

/**
 * @brief 触发 IO_UPDATE 脉冲，将缓冲寄存器生效到工作路径。
 * @param dev 设备对象指针。
 * @return 执行状态，未初始化时返回 AD9910_STATUS_NOT_INITIALIZED。
 */
ad9910_status_t AD9910_IOUpdate(ad9910_t *dev);

/**
 * @brief I/O 串口状态机复位。
 * @param dev 设备对象指针。
 * @return 执行状态，未配置 IO_RESET 引脚时返回 AD9910_STATUS_NOT_SUPPORTED。
 * @note 该引脚仅复位串行 I/O 控制状态机，不会清除 AD9910 功能寄存器。
 */
ad9910_status_t AD9910_IOReset(ad9910_t *dev);

/**
 * @brief 向指定 AD9910 寄存器写入数据。
 * @param dev 设备对象指针。
 * @param reg_addr 寄存器地址。
 * @param data 待写入数据缓冲区指针。
 * @param len 写入字节数，需与寄存器长度匹配。
 * @return 执行状态，参数不合法时返回 AD9910_STATUS_INVALID_PARAM。
 */
ad9910_status_t AD9910_WriteRegister(ad9910_t *dev, uint8_t reg_addr, const uint8_t *data, uint16_t len);
#if (AD9910_ENABLE_READBACK == 1U)
/**
 * @brief 从指定 AD9910 寄存器读出数据。
 * @param dev 设备对象指针。
 * @param reg_addr 寄存器地址。
 * @param data 读回数据缓冲区指针。
 * @param len 读回字节数，需与寄存器长度匹配。
 * @return 执行状态，不支持读回时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_ReadRegister(ad9910_t *dev, uint8_t reg_addr, uint8_t *data, uint16_t len);
#endif

/**
 * @brief 配置系统时钟相关参数并写入 CFR3。
 * @param dev 设备对象指针。
 * @param cfg 系统时钟配置指针。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetSysClockConfig(ad9910_t *dev, const ad9910_sysclk_config_t *cfg, uint8_t auto_io_update);

/**
 * @brief 获取当前缓存的系统时钟配置。
 * @param dev 设备对象指针。
 * @param cfg 输出配置指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetSysClockConfig(const ad9910_t *dev, ad9910_sysclk_config_t *cfg);

/**
 * @brief 将输出频率（Hz）换算为 FTW 字。
 * @param frequency_hz 目标频率，单位 Hz。
 * @param sysclk_hz 系统时钟，单位 Hz。
 * @return 32 位 FTW 字。
 */
uint32_t AD9910_FrequencyToFTW(double frequency_hz, double sysclk_hz);

/**
 * @brief 将 FTW 字换算为输出频率（Hz）。
 * @param ftw 32 位 FTW 字。
 * @param sysclk_hz 系统时钟，单位 Hz。
 * @return 换算后的频率值，单位 Hz。
 */
double AD9910_FTWToFrequency(uint32_t ftw, double sysclk_hz);

/**
 * @brief 将相位角（度）换算为 POW 字。
 * @param phase_deg 相位角，单位度。
 * @return 16 位 POW 字。
 */
uint16_t AD9910_PhaseDegToPOW(double phase_deg);

/**
 * @brief 将 POW 字换算为相位角（度）。
 * @param pow_word 16 位 POW 字。
 * @return 相位角，单位度。
 */
double AD9910_POWToPhaseDeg(uint16_t pow_word);

/**
 * @brief 将归一化幅度换算为 ASF 字。
 * @param amplitude_scale 归一化幅度，推荐范围 [0.0, 1.0]。
 * @return 14 位 ASF 字。
 */
uint16_t AD9910_AmplitudeScaleToASF(double amplitude_scale);

/**
 * @brief 将 ASF 字换算为归一化幅度。
 * @param asf_word ASF 字。
 * @return 归一化幅度值。
 */
double AD9910_ASFToAmplitudeScale(uint16_t asf_word);

/**
 * @brief 设置单音频率调谐字 FTW。
 * @param dev 设备对象指针。
 * @param ftw 32 位 FTW 字。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetFTW(ad9910_t *dev, uint32_t ftw, uint8_t auto_io_update);

/**
 * @brief 设置单音相位偏移字 POW。
 * @param dev 设备对象指针。
 * @param pow_word 16 位 POW 字。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetPOW(ad9910_t *dev, uint16_t pow_word, uint8_t auto_io_update);

/**
 * @brief 设置单音幅度比例字 ASF。
 * @param dev 设备对象指针。
 * @param asf_word 14 位 ASF 字。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态，ASF 越界时返回 AD9910_STATUS_INVALID_PARAM。
 */
ad9910_status_t AD9910_SetASF(ad9910_t *dev, uint16_t asf_word, uint8_t auto_io_update);

/**
 * @brief 以 FTW/POW/ASF 一次性配置单音输出参数。
 * @param dev 设备对象指针。
 * @param tone 单音参数指针。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetSingleTone(ad9910_t *dev, const ad9910_single_tone_t *tone, uint8_t auto_io_update);

/**
 * @brief 以工程单位（Hz/度/归一化幅度）配置单音输出。
 * @param dev 设备对象指针。
 * @param frequency_hz 目标频率，单位 Hz。
 * @param phase_deg 目标相位，单位度。
 * @param amplitude_scale 目标归一化幅度，范围 [0.0, 1.0]。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetSingleToneHz(ad9910_t *dev,
  double frequency_hz,
  double phase_deg,
  double amplitude_scale,
  uint8_t auto_io_update);

/**
 * @brief 读取当前单音配置（FTW/POW/ASF）。
 * @param dev 设备对象指针。
 * @param tone 输出单音参数指针。
 * @return 执行状态，不支持读回时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_GetSingleTone(ad9910_t *dev, ad9910_single_tone_t *tone);

/**
 * @brief 写入指定 Profile 寄存器组。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param word Profile 数据指针。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ProgramProfile(ad9910_t *dev,
  ad9910_profile_t profile,
  const ad9910_profile_word_t *word,
  uint8_t auto_io_update);

/**
 * @brief 读取指定 Profile 寄存器组。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param word 输出 Profile 数据指针。
 * @return 执行状态，不支持读回时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_ReadProfile(ad9910_t *dev, ad9910_profile_t profile, ad9910_profile_word_t *word);

/**
 * @brief 通过 PROFILE[2:0] 引脚选择 Profile。
 * @param dev 设备对象指针。
 * @param profile 目标 Profile 编号。
 * @return 执行状态，未配置 Profile 引脚时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_SelectProfilePins(ad9910_t *dev, ad9910_profile_t profile);

/**
 * @brief 设置辅助 DAC 满量程电流控制码。
 * @param dev 设备对象指针。
 * @param fsc_code 辅助 DAC FSC 代码。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetAuxDacFsc(ad9910_t *dev, uint8_t fsc_code, uint8_t auto_io_update);

/**
 * @brief 获取缓存的辅助 DAC FSC 代码。
 * @param dev 设备对象指针。
 * @param fsc_code 输出 FSC 代码指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetAuxDacFsc(ad9910_t *dev, uint8_t *fsc_code);

/**
 * @brief 配置电源管理相关位（CFR1）。
 * @param dev 设备对象指针。
 * @param cfg 电源控制配置指针。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetPowerDown(ad9910_t *dev,
  const ad9910_power_down_config_t *cfg,
  uint8_t auto_io_update);

/**
 * @brief 获取当前缓存的电源管理配置。
 * @param dev 设备对象指针。
 * @param cfg 输出电源控制配置指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetPowerDown(ad9910_t *dev, ad9910_power_down_config_t *cfg);

/**
 * @brief 配置数字斜坡发生器（DRG）参数。
 * @param dev 设备对象指针。
 * @param cfg DRG 配置指针。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ConfigureDRG(ad9910_t *dev,
  const ad9910_drg_config_t *cfg,
  uint8_t auto_io_update);

/**
 * @brief 使能或关闭 DRG。
 * @param dev 设备对象指针。
 * @param enable 1=使能，0=关闭。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_EnableDRG(ad9910_t *dev, uint8_t enable, uint8_t auto_io_update);

/* UPDATE1 新增引脚控制接口 */
/**
 * @brief 设置 DR_HOLD 控制引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平，非 0 为高电平。
 * @return 执行状态，未配置引脚时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_SetDRHold(ad9910_t *dev, uint8_t level);

/**
 * @brief 设置 DR_CTL 控制引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平，非 0 为高电平。
 * @return 执行状态，未配置引脚时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_SetDRCtl(ad9910_t *dev, uint8_t level);

/**
 * @brief 读取 DR_OVER 状态引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平指针，非 0 表示高电平。
 * @return 执行状态，未配置引脚时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_GetDROver(ad9910_t *dev, uint8_t *level);

/**
 * @brief 设置 OSK 控制引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平，非 0 为高电平。
 * @return 执行状态，未配置引脚时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_SetOSKPin(ad9910_t *dev, uint8_t level);

/**
 * @brief 配置手动 OSK 外部控制。
 * @param dev 设备对象指针。
 * @param enable 1=启用手动 OSK（CFR1[9]=1, CFR1[8]=0, CFR1[23]=1），0=关闭。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ConfigureManualOSK(ad9910_t *dev, uint8_t enable, uint8_t auto_io_update);

/**
 * @brief 向 RAM 连续写入 32 位数据字。
 * @param dev 设备对象指针。
 * @param words 待写入 RAM 数据指针。
 * @param word_count 写入字数。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_WriteRam(ad9910_t *dev,
  const uint32_t *words,
  uint16_t word_count,
  uint8_t auto_io_update);
#if (AD9910_ENABLE_READBACK == 1U)
/**
 * @brief 从 RAM 连续读取 32 位数据字。
 * @param dev 设备对象指针。
 * @param words 输出 RAM 数据指针。
 * @param word_count 读取字数。
 * @return 执行状态。
 * @note 需开启读回并满足硬件/引脚条件。
 */
ad9910_status_t AD9910_ReadRam(ad9910_t *dev,
  uint32_t *words,
  uint16_t word_count);
#endif

/**
 * @brief 写入指定 Profile 的 RAM 播放控制字。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param cfg RAM Profile 配置指针。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ProgramRamProfile(ad9910_t *dev,
  ad9910_profile_t profile,
  const ad9910_ram_profile_config_t *cfg,
  uint8_t auto_io_update);

/**
 * @brief 读取指定 Profile 的 RAM 播放控制字。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param cfg 输出 RAM Profile 配置指针。
 * @return 执行状态，不支持读回时返回 AD9910_STATUS_NOT_SUPPORTED。
 */
ad9910_status_t AD9910_ReadRamProfile(ad9910_t *dev,
  ad9910_profile_t profile,
  ad9910_ram_profile_config_t *cfg);

/**
 * @brief 配置 RAM 功能开关与播放目标（CFR1[31], CFR1[30:29]）。
 * @param dev 设备对象指针。
 * @param enable 1=使能 RAM 功能，0=关闭。
 * @param destination RAM 播放目标。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ConfigureRamMode(ad9910_t *dev,
  uint8_t enable,
  ad9910_ram_destination_t destination,
  uint8_t auto_io_update);

/**
 * @brief 生成常用波形码表（1024 点，范围 0~16383）。
 * @param cfg 波形配置指针。
 * @param out_codes 输出波形数组，长度需为 AD9910_RAM_POINT_COUNT。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GenerateWaveform(const ad9910_waveform_config_t *cfg,
  uint16_t out_codes[AD9910_RAM_POINT_COUNT]);

/**
 * @brief 将 14 位幅度码表装载到 AD9910 RAM（1024 点）。
 * @param dev 设备对象指针。
 * @param amp_codes 14 位波形码表，长度 AD9910_RAM_POINT_COUNT。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_LoadRamWaveform(ad9910_t *dev,
  const uint16_t amp_codes[AD9910_RAM_POINT_COUNT],
  uint8_t auto_io_update);

/**
 * @brief 一键生成并装载常用波形到 AD9910 RAM。
 * @param dev 设备对象指针。
 * @param cfg 波形配置指针。
 * @param auto_io_update 是否自动触发 IO_UPDATE，非 0 表示触发。
 * @return 执行状态。
 */
ad9910_status_t AD9910_LoadBuiltInWaveform(ad9910_t *dev,
  const ad9910_waveform_config_t *cfg,
  uint8_t auto_io_update);

#ifdef __cplusplus
}
#endif

#endif /* AD9910_H */
