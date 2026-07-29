#include "ad9910.h"

#include <string.h>
#include <math.h>

/* ========================= 内部宏与兼容定义 ========================= */

#if (AD9910_SPI_MODE == AD9910_SPI_MODE_HARDWARE)
typedef SPI_HandleTypeDef ad9910_spi_handle_t;
#endif

#define AD9910_GPIO_SET(port_, pin_)    ((port_)->BSRR = AD9910_GPIO_BSRR_SET(pin_))
#define AD9910_GPIO_CLR(port_, pin_)    ((port_)->BSRR = AD9910_GPIO_BSRR_CLR(pin_))

#define AD9910_PI                        (3.14159265358979323846)

/* ========================= 内置波形常量表（Flash驻留） ========================= */

#define AD9910_TRI_HALF_MAX 512U

#define AD9910_TRI_VAL(i_) ((i_) < AD9910_TRI_HALF_MAX ? ((uint16_t)(((i_) * AD9910_ASF_MAX) / (AD9910_TRI_HALF_MAX - 1U))) : ((uint16_t)((((AD9910_RAM_POINT_COUNT - 1U) - (i_)) * AD9910_ASF_MAX) / (AD9910_TRI_HALF_MAX - 1U))))

#define AD9910_SQ_VAL(i_)  ((i_) < AD9910_TRI_HALF_MAX ? (0U) : (AD9910_ASF_MAX))

#define AD9910_SINC_VAL(i_) ((uint16_t)((i_) * 16U))

#define AD9910_TRI_16(base_) \
  AD9910_TRI_VAL((base_) + 0U), AD9910_TRI_VAL((base_) + 1U), AD9910_TRI_VAL((base_) + 2U), AD9910_TRI_VAL((base_) + 3U), \
  AD9910_TRI_VAL((base_) + 4U), AD9910_TRI_VAL((base_) + 5U), AD9910_TRI_VAL((base_) + 6U), AD9910_TRI_VAL((base_) + 7U), \
  AD9910_TRI_VAL((base_) + 8U), AD9910_TRI_VAL((base_) + 9U), AD9910_TRI_VAL((base_) + 10U), AD9910_TRI_VAL((base_) + 11U), \
  AD9910_TRI_VAL((base_) + 12U), AD9910_TRI_VAL((base_) + 13U), AD9910_TRI_VAL((base_) + 14U), AD9910_TRI_VAL((base_) + 15U)

#define AD9910_SQ_16(base_) \
  AD9910_SQ_VAL((base_) + 0U), AD9910_SQ_VAL((base_) + 1U), AD9910_SQ_VAL((base_) + 2U), AD9910_SQ_VAL((base_) + 3U), \
  AD9910_SQ_VAL((base_) + 4U), AD9910_SQ_VAL((base_) + 5U), AD9910_SQ_VAL((base_) + 6U), AD9910_SQ_VAL((base_) + 7U), \
  AD9910_SQ_VAL((base_) + 8U), AD9910_SQ_VAL((base_) + 9U), AD9910_SQ_VAL((base_) + 10U), AD9910_SQ_VAL((base_) + 11U), \
  AD9910_SQ_VAL((base_) + 12U), AD9910_SQ_VAL((base_) + 13U), AD9910_SQ_VAL((base_) + 14U), AD9910_SQ_VAL((base_) + 15U)

#define AD9910_SINC_16(base_) \
  AD9910_SINC_VAL((base_) + 0U), AD9910_SINC_VAL((base_) + 1U), AD9910_SINC_VAL((base_) + 2U), AD9910_SINC_VAL((base_) + 3U), \
  AD9910_SINC_VAL((base_) + 4U), AD9910_SINC_VAL((base_) + 5U), AD9910_SINC_VAL((base_) + 6U), AD9910_SINC_VAL((base_) + 7U), \
  AD9910_SINC_VAL((base_) + 8U), AD9910_SINC_VAL((base_) + 9U), AD9910_SINC_VAL((base_) + 10U), AD9910_SINC_VAL((base_) + 11U), \
  AD9910_SINC_VAL((base_) + 12U), AD9910_SINC_VAL((base_) + 13U), AD9910_SINC_VAL((base_) + 14U), AD9910_SINC_VAL((base_) + 15U)

#define AD9910_WAVE_256_TRI \
  AD9910_TRI_16(0U), AD9910_TRI_16(16U), AD9910_TRI_16(32U), AD9910_TRI_16(48U), \
  AD9910_TRI_16(64U), AD9910_TRI_16(80U), AD9910_TRI_16(96U), AD9910_TRI_16(112U), \
  AD9910_TRI_16(128U), AD9910_TRI_16(144U), AD9910_TRI_16(160U), AD9910_TRI_16(176U), \
  AD9910_TRI_16(192U), AD9910_TRI_16(208U), AD9910_TRI_16(224U), AD9910_TRI_16(240U)

#define AD9910_WAVE_256_SQ \
  AD9910_SQ_16(0U), AD9910_SQ_16(16U), AD9910_SQ_16(32U), AD9910_SQ_16(48U), \
  AD9910_SQ_16(64U), AD9910_SQ_16(80U), AD9910_SQ_16(96U), AD9910_SQ_16(112U), \
  AD9910_SQ_16(128U), AD9910_SQ_16(144U), AD9910_SQ_16(160U), AD9910_SQ_16(176U), \
  AD9910_SQ_16(192U), AD9910_SQ_16(208U), AD9910_SQ_16(224U), AD9910_SQ_16(240U)

#define AD9910_WAVE_256_SINC \
  AD9910_SINC_16(0U), AD9910_SINC_16(16U), AD9910_SINC_16(32U), AD9910_SINC_16(48U), \
  AD9910_SINC_16(64U), AD9910_SINC_16(80U), AD9910_SINC_16(96U), AD9910_SINC_16(112U), \
  AD9910_SINC_16(128U), AD9910_SINC_16(144U), AD9910_SINC_16(160U), AD9910_SINC_16(176U), \
  AD9910_SINC_16(192U), AD9910_SINC_16(208U), AD9910_SINC_16(224U), AD9910_SINC_16(240U)

static const uint16_t g_ad9910_wave_triangle[AD9910_RAM_POINT_COUNT] = {
  AD9910_WAVE_256_TRI, AD9910_WAVE_256_TRI, AD9910_WAVE_256_TRI, AD9910_WAVE_256_TRI
};

static const uint16_t g_ad9910_wave_square[AD9910_RAM_POINT_COUNT] = {
  AD9910_WAVE_256_SQ, AD9910_WAVE_256_SQ, AD9910_WAVE_256_SQ, AD9910_WAVE_256_SQ
};

static const uint16_t g_ad9910_wave_sinc[AD9910_RAM_POINT_COUNT] = {
  AD9910_WAVE_256_SINC, AD9910_WAVE_256_SINC, AD9910_WAVE_256_SINC, AD9910_WAVE_256_SINC
};

/* ========================= 内部辅助函数 ========================= */

/**
 * @brief 将 HAL 返回状态映射为驱动状态码。
 * @param hal_status HAL 层状态值。
 * @return 对应的 ad9910_status_t 状态码。
 */
static ad9910_status_t ad9910_from_hal(HAL_StatusTypeDef hal_status)
{
  switch (hal_status)
  {
    case HAL_OK:
      return AD9910_STATUS_OK;
    case HAL_TIMEOUT:
      return AD9910_STATUS_TIMEOUT;
    default:
      return AD9910_STATUS_ERROR;
  }
}

/**
 * @brief 检查 GPIO 描述是否有效。
 * @param gpio GPIO 描述指针。
 * @return 1 表示有效，0 表示无效。
 */
static uint8_t ad9910_pin_valid(const ad9910_gpio_t *gpio)
{
  return (uint8_t)((gpio != NULL) && (gpio->port != NULL) && (gpio->pin != 0U));
}

/**
 * @brief 检查 Profile 选择引脚是否全部有效。
 * @param dev 设备对象指针。
 * @return 1 表示有效，0 表示无效。
 */
static uint8_t ad9910_profile_pins_valid(const ad9910_t *dev)
{
  return (uint8_t)((dev != NULL) &&
    (ad9910_pin_valid(&dev->cfg.profile0) != 0U) &&
    (ad9910_pin_valid(&dev->cfg.profile1) != 0U) &&
    (ad9910_pin_valid(&dev->cfg.profile2) != 0U));
}

/**
 * @brief 检查软件 SPI 必需引脚是否有效。
 * @param dev 设备对象指针。
 * @return 1 表示有效，0 表示无效。
 */
static uint8_t ad9910_soft_spi_pins_valid(const ad9910_t *dev)
{
  return (uint8_t)((dev != NULL) &&
    (ad9910_pin_valid(&dev->cfg.soft_sclk) != 0U) &&
    (ad9910_pin_valid(&dev->cfg.soft_sdio) != 0U));
}

/**
 * @brief 以 NOP 方式执行固定周期延时。
 * @param cycles 延时循环次数。
 */
static void ad9910_delay_cycles(uint32_t cycles)
{
  volatile uint32_t i;
  for (i = 0U; i < cycles; ++i)
  {
    __NOP();
  }
}

/**
 * @brief 执行软件 SPI 半周期延时。
 */
static void ad9910_soft_spi_delay(void)
{
  ad9910_delay_cycles(AD9910_SOFT_SPI_DELAY_CYCLES);
}

/**
 * @brief 将浮点数四舍五入并饱和转换为 uint32_t。
 * @param value 输入浮点值。
 * @return 转换后的 32 位无符号整数。
 */
static uint32_t ad9910_round_to_u32(double value)
{
  if (value <= 0.0)
  {
    return 0UL;
  }
  if (value >= 4294967295.0)
  {
    return 0xFFFFFFFFUL;
  }
  return (uint32_t)(value + 0.5);
}

/**
 * @brief 将浮点数四舍五入并饱和转换为 uint16_t。
 * @param value 输入浮点值。
 * @return 转换后的 16 位无符号整数。
 */
static uint16_t ad9910_round_to_u16(double value)
{
  if (value <= 0.0)
  {
    return 0U;
  }
  if (value >= 65535.0)
  {
    return 0xFFFFU;
  }
  return (uint16_t)(value + 0.5);
}

/**
 * @brief 将 32 位整数按大端格式打包到字节数组。
 * @param value 输入 32 位整数。
 * @param out 输出 4 字节缓冲区。
 */
static void ad9910_pack_u32_be(uint32_t value, uint8_t out[4])
{
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

/**
 * @brief 将大端 4 字节数据解包为 32 位整数。
 * @param in 输入 4 字节缓冲区。
 * @return 解包后的 32 位无符号整数。
 */
static uint32_t ad9910_unpack_u32_be(const uint8_t in[4])
{
  return (((uint32_t)in[0]) << 24) |
    (((uint32_t)in[1]) << 16) |
    (((uint32_t)in[2]) << 8) |
    ((uint32_t)in[3]);
}

/**
 * @brief 将 64 位整数按大端格式打包到字节数组。
 * @param value 输入 64 位整数。
 * @param out 输出 8 字节缓冲区。
 */
static void ad9910_pack_u64_be(uint64_t value, uint8_t out[8])
{
  out[0] = (uint8_t)(value >> 56);
  out[1] = (uint8_t)(value >> 48);
  out[2] = (uint8_t)(value >> 40);
  out[3] = (uint8_t)(value >> 32);
  out[4] = (uint8_t)(value >> 24);
  out[5] = (uint8_t)(value >> 16);
  out[6] = (uint8_t)(value >> 8);
  out[7] = (uint8_t)value;
}

/**
 * @brief 将大端 8 字节数据解包为 64 位整数。
 * @param in 输入 8 字节缓冲区。
 * @return 解包后的 64 位无符号整数。
 */
static uint64_t ad9910_unpack_u64_be(const uint8_t in[8])
{
  return (((uint64_t)in[0]) << 56) |
    (((uint64_t)in[1]) << 48) |
    (((uint64_t)in[2]) << 40) |
    (((uint64_t)in[3]) << 32) |
    (((uint64_t)in[4]) << 24) |
    (((uint64_t)in[5]) << 16) |
    (((uint64_t)in[6]) << 8) |
    ((uint64_t)in[7]);
}

/**
 * @brief 获取寄存器地址对应的数据长度。
 * @param reg_addr 寄存器地址。
 * @return 长度（字节），未知地址返回 0。
 */
static uint16_t ad9910_register_length(uint8_t reg_addr)
{
  switch (reg_addr)
  {
    case AD9910_REG_CFR1: return AD9910_REG_LEN_CFR1;
    case AD9910_REG_CFR2: return AD9910_REG_LEN_CFR2;
    case AD9910_REG_CFR3: return AD9910_REG_LEN_CFR3;
    case AD9910_REG_AUX_DAC: return AD9910_REG_LEN_AUX_DAC;
    case AD9910_REG_IO_UPDATE_RATE: return AD9910_REG_LEN_IO_UPDATE_RATE;
    case AD9910_REG_FTW: return AD9910_REG_LEN_FTW;
    case AD9910_REG_POW: return AD9910_REG_LEN_POW;
    case AD9910_REG_ASF: return AD9910_REG_LEN_ASF;
    case AD9910_REG_MULTICHIP_SYNC: return AD9910_REG_LEN_MULTICHIP_SYNC;
    case AD9910_REG_DIGITAL_RAMP_LIMIT: return AD9910_REG_LEN_DIGITAL_RAMP_LIMIT;
    case AD9910_REG_DIGITAL_RAMP_STEP: return AD9910_REG_LEN_DIGITAL_RAMP_STEP;
    case AD9910_REG_DIGITAL_RAMP_RATE: return AD9910_REG_LEN_DIGITAL_RAMP_RATE;
    case AD9910_REG_RAM: return AD9910_REG_LEN_RAM_WORD;
    default:
      if ((reg_addr >= AD9910_REG_PROFILE0) && (reg_addr <= AD9910_REG_PROFILE7))
      {
        return AD9910_REG_LEN_PROFILE;
      }
      return 0U;
  }
}

/**
 * @brief 将 16 位幅度码（0~16383）封装为 AD9910 RAM 32 位字。
 * @param code14 输入 14 位幅度码。
 * @return RAM 32 位字，幅度放在 bit[31:18]。
 */
static uint32_t ad9910_pack_ram_amp_word(uint16_t code14)
{
  uint16_t sat = code14;
  if (sat > AD9910_ASF_MAX)
  {
    sat = AD9910_ASF_MAX;
  }
  return ((uint32_t)sat) << 18U;
}

/**
 * @brief 对 14 位码值进行上下限约束。
 * @param value 输入码值。
 * @param min_code 下限。
 * @param max_code 上限。
 * @return 约束后的码值。
 */
static uint16_t ad9910_clamp_code(uint16_t value, uint16_t min_code, uint16_t max_code)
{
  if (value < min_code)
  {
    return min_code;
  }
  if (value > max_code)
  {
    return max_code;
  }
  return value;
}

/**
 * @brief 根据类型返回内置常量波形表。
 * @param type 波形类型。
 * @param table 输出常量表指针。
 * @return 执行状态。
 */
static ad9910_status_t ad9910_get_builtin_table(ad9910_waveform_type_t type, const uint16_t **table)
{
  if (table == NULL)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  switch (type)
  {
    case AD9910_WAVE_TRIANGLE:
      *table = g_ad9910_wave_triangle;
      return AD9910_STATUS_OK;
    case AD9910_WAVE_SQUARE:
      *table = g_ad9910_wave_square;
      return AD9910_STATUS_OK;
    case AD9910_WAVE_SINC:
      *table = g_ad9910_wave_sinc;
      return AD9910_STATUS_OK;
    default:
      return AD9910_STATUS_INVALID_PARAM;
  }
}

/**
 * @brief 在写寄存器成功后更新本地影子寄存器缓存。
 * @param dev 设备对象指针。
 * @param reg_addr 已写入的寄存器地址。
 * @param data 寄存器数据缓冲区。
 * @return 执行状态。
 */
static ad9910_status_t ad9910_update_cached_register(ad9910_t *dev, uint8_t reg_addr, const uint8_t *data)
{
  if ((dev == NULL) || (data == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  switch (reg_addr)
  {
    case AD9910_REG_CFR1:
      dev->cfr1_shadow = ad9910_unpack_u32_be(data);
      break;
    case AD9910_REG_CFR2:
      dev->cfr2_shadow = ad9910_unpack_u32_be(data);
      break;
    case AD9910_REG_CFR3:
      dev->cfr3_shadow = ad9910_unpack_u32_be(data);
      break;
    case AD9910_REG_AUX_DAC:
      dev->aux_dac_fsc = data[3];
      break;
    default:
      break;
  }

  return AD9910_STATUS_OK;
}

/* ========================= SPI 抽象层（UPDATE1） ========================= */

/**
 * @brief 拉低片选信号，进入 SPI 传输窗口。
 * @param dev 设备对象指针。
 * @return 执行状态。
 */
static ad9910_status_t ad9910_cs_active(const ad9910_t *dev)
{
  if (ad9910_pin_valid(&dev->cfg.cs) == 0U)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  AD9910_GPIO_CLR(dev->cfg.cs.port, dev->cfg.cs.pin);
  return AD9910_STATUS_OK;
}

/**
 * @brief 拉高片选信号，结束 SPI 传输窗口。
 * @param dev 设备对象指针。
 * @return 执行状态。
 */
static ad9910_status_t ad9910_cs_idle(const ad9910_t *dev)
{
  if (ad9910_pin_valid(&dev->cfg.cs) == 0U)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  AD9910_GPIO_SET(dev->cfg.cs.port, dev->cfg.cs.pin);
  return AD9910_STATUS_OK;
}

/**
 * @brief 软件 SPI 将 SCLK 置高。
 * @param dev 设备对象指针。
 */
static void ad9910_soft_sclk_high(const ad9910_t *dev)
{
  AD9910_GPIO_SET(dev->cfg.soft_sclk.port, dev->cfg.soft_sclk.pin);
}

/**
 * @brief 软件 SPI 将 SCLK 置低。
 * @param dev 设备对象指针。
 */
static void ad9910_soft_sclk_low(const ad9910_t *dev)
{
  AD9910_GPIO_CLR(dev->cfg.soft_sclk.port, dev->cfg.soft_sclk.pin);
}

/**
 * @brief 软件 SPI 将 SDIO 置高。
 * @param dev 设备对象指针。
 */
static void ad9910_soft_sdio_high(const ad9910_t *dev)
{
  AD9910_GPIO_SET(dev->cfg.soft_sdio.port, dev->cfg.soft_sdio.pin);
}

/**
 * @brief 软件 SPI 将 SDIO 置低。
 * @param dev 设备对象指针。
 */
static void ad9910_soft_sdio_low(const ad9910_t *dev)
{
  AD9910_GPIO_CLR(dev->cfg.soft_sdio.port, dev->cfg.soft_sdio.pin);
}

/**
 * @brief 读取软件 SPI 的 SDO 输入电平。
 * @param dev 设备对象指针。
 * @return 读到高电平返回 1，否则返回 0。
 */
static uint8_t ad9910_soft_sdo_read(const ad9910_t *dev)
{
  if (ad9910_pin_valid(&dev->cfg.soft_sdo) == 0U)
  {
    return 0U;
  }
  return (uint8_t)((dev->cfg.soft_sdo.port->IDR & dev->cfg.soft_sdo.pin) != 0U);
}

/**
 * @brief 软件 SPI 发送 1 字节数据（MSB 先行）。
 * @param dev 设备对象指针。
 * @param value 待发送字节。
 */
static void ad9910_soft_spi_write_byte(const ad9910_t *dev, uint8_t value)
{
  uint8_t bit;
  for (bit = 0U; bit < 8U; ++bit)
  {
    if ((value & 0x80U) != 0U)
    {
      ad9910_soft_sdio_high(dev);
    }
    else
    {
      ad9910_soft_sdio_low(dev);
    }

    ad9910_soft_spi_delay();
    ad9910_soft_sclk_high(dev);
    ad9910_soft_spi_delay();
    ad9910_soft_sclk_low(dev);
    value <<= 1;
  }
}

/**
 * @brief 软件 SPI 接收 1 字节数据（MSB 先行）。
 * @param dev 设备对象指针。
 * @return 读取到的字节值。
 */
static uint8_t ad9910_soft_spi_read_byte(const ad9910_t *dev)
{
  uint8_t bit;
  uint8_t value = 0U;

  for (bit = 0U; bit < 8U; ++bit)
  {
    value <<= 1;
    ad9910_soft_sclk_high(dev);
    ad9910_soft_spi_delay();
    if (ad9910_soft_sdo_read(dev) != 0U)
    {
      value |= 1U;
    }
    ad9910_soft_sclk_low(dev);
    ad9910_soft_spi_delay();
  }

  return value;
}

/**
 * @brief SPI 抽象层发送接口（软/硬 SPI 自动分发）。
 * @param dev 设备对象指针。
 * @param data 发送数据指针。
 * @param len 发送字节数。
 * @return 执行状态。
 */
static ad9910_status_t ad9910_spi_tx(ad9910_t *dev, const uint8_t *data, uint16_t len)
{
#if (AD9910_SPI_MODE == AD9910_SPI_MODE_HARDWARE)
  ad9910_spi_handle_t *hspi;
  if ((dev == NULL) || (data == NULL) || (len == 0U) || (dev->cfg.hspi == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  hspi = (ad9910_spi_handle_t *)dev->cfg.hspi;
#if (AD9910_SPI_USE_DMA == 1U)
  return ad9910_from_hal(HAL_SPI_Transmit_DMA(hspi, (uint8_t *)data, len));
#else
  return ad9910_from_hal(HAL_SPI_Transmit(hspi, (uint8_t *)data, len, dev->cfg.spi_timeout_ms));
#endif
#else
  uint16_t i;
  if ((dev == NULL) || (data == NULL) || (len == 0U))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  if (ad9910_soft_spi_pins_valid(dev) == 0U)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  for (i = 0U; i < len; ++i)
  {
    ad9910_soft_spi_write_byte(dev, data[i]);
  }
  return AD9910_STATUS_OK;
#endif
}

/**
 * @brief SPI 抽象层接收接口（软/硬 SPI 自动分发）。
 * @param dev 设备对象指针。
 * @param data 接收缓冲区指针。
 * @param len 接收字节数。
 * @return 执行状态。
 */
static ad9910_status_t ad9910_spi_rx(ad9910_t *dev, uint8_t *data, uint16_t len)
{
#if (AD9910_SPI_MODE == AD9910_SPI_MODE_HARDWARE)
  ad9910_spi_handle_t *hspi;
  if ((dev == NULL) || (data == NULL) || (len == 0U) || (dev->cfg.hspi == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  hspi = (ad9910_spi_handle_t *)dev->cfg.hspi;
#if (AD9910_SPI_USE_DMA == 1U)
  return ad9910_from_hal(HAL_SPI_Receive_DMA(hspi, data, len));
#else
  return ad9910_from_hal(HAL_SPI_Receive(hspi, data, len, dev->cfg.spi_timeout_ms));
#endif
#else
  uint16_t i;
  if ((dev == NULL) || (data == NULL) || (len == 0U))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  if ((ad9910_soft_spi_pins_valid(dev) == 0U) || (ad9910_pin_valid(&dev->cfg.soft_sdo) == 0U))
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  for (i = 0U; i < len; ++i)
  {
    data[i] = ad9910_soft_spi_read_byte(dev);
  }
  return AD9910_STATUS_OK;
#endif
}

/**
 * @brief 检查设备对象及关键配置是否有效。
 * @param dev 设备对象指针。
 * @return 1 表示有效，0 表示无效。
 */
static uint8_t ad9910_device_valid(const ad9910_t *dev)
{
  if (dev == NULL)
  {
    return 0U;
  }

  if ((ad9910_pin_valid(&dev->cfg.cs) == 0U) ||
      (ad9910_pin_valid(&dev->cfg.reset) == 0U) ||
      (ad9910_pin_valid(&dev->cfg.io_update) == 0U))
  {
    return 0U;
  }

  if (dev->cfg.sysclk.sys_clk_hz == 0ULL)
  {
    return 0U;
  }

#if (AD9910_SPI_MODE == AD9910_SPI_MODE_HARDWARE)
  if (dev->cfg.hspi == NULL)
  {
    return 0U;
  }
#else
  if (ad9910_soft_spi_pins_valid(dev) == 0U)
  {
    return 0U;
  }
#endif

  return 1U;
}

/* ========================= 基础控制接口 ========================= */

/**
 * @brief 初始化 AD9910 设备并下发默认关键配置。
 * @param dev 设备对象指针。
 * @param init 初始化参数指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_Init(ad9910_t *dev, const ad9910_init_t *init)
{
  ad9910_init_t default_cfg = AD9910_INIT_DEFAULT();
  uint8_t cfr_buf[4];
  ad9910_status_t status;

  if ((dev == NULL) || (init == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  memset(dev, 0, sizeof(*dev));
  dev->cfg = *init;

  if (dev->cfg.spi_timeout_ms == 0U)
  {
    dev->cfg.spi_timeout_ms = default_cfg.spi_timeout_ms;
  }

  if (ad9910_device_valid(dev) == 0U)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  dev->cfr1_shadow = AD9910_CFR1_DEFAULT_VALUE | AD9910_BIT_U32(AD9910_CFR1_SELECT_DDS_SINE_BIT);
  dev->cfr2_shadow = AD9910_CFR2_DEFAULT_VALUE |
    AD9910_BIT_U32(AD9910_CFR2_AMP_FROM_PROFILE_ENABLE_BIT);
  dev->cfr3_shadow = AD9910_CFR3_DEFAULT_VALUE;
  dev->aux_dac_fsc = 0x7FU;

  (void)ad9910_cs_idle(dev);
  AD9910_GPIO_CLR(dev->cfg.io_update.port, dev->cfg.io_update.pin);
  AD9910_GPIO_CLR(dev->cfg.reset.port, dev->cfg.reset.pin);

  if (ad9910_pin_valid(&dev->cfg.io_reset) != 0U)
  {
    AD9910_GPIO_CLR(dev->cfg.io_reset.port, dev->cfg.io_reset.pin);
  }

  if (ad9910_profile_pins_valid(dev) != 0U)
  {
    AD9910_GPIO_CLR(dev->cfg.profile0.port, dev->cfg.profile0.pin);
    AD9910_GPIO_CLR(dev->cfg.profile1.port, dev->cfg.profile1.pin);
    AD9910_GPIO_CLR(dev->cfg.profile2.port, dev->cfg.profile2.pin);
  }

  if (ad9910_pin_valid(&dev->cfg.dr_hold) != 0U)
  {
    AD9910_GPIO_CLR(dev->cfg.dr_hold.port, dev->cfg.dr_hold.pin);
  }
  if (ad9910_pin_valid(&dev->cfg.dr_ctl) != 0U)
  {
    AD9910_GPIO_CLR(dev->cfg.dr_ctl.port, dev->cfg.dr_ctl.pin);
  }
  if (ad9910_pin_valid(&dev->cfg.osk) != 0U)
  {
    AD9910_GPIO_CLR(dev->cfg.osk.port, dev->cfg.osk.pin);
  }

#if (AD9910_SPI_MODE == AD9910_SPI_MODE_SOFTWARE)
  AD9910_GPIO_CLR(dev->cfg.soft_sclk.port, dev->cfg.soft_sclk.pin);
  AD9910_GPIO_CLR(dev->cfg.soft_sdio.port, dev->cfg.soft_sdio.pin);
#endif

  dev->initialized = 1U;

  status = AD9910_Reset(dev);
  if (status != AD9910_STATUS_OK)
  {
    dev->initialized = 0U;
    return status;
  }

  ad9910_pack_u32_be(dev->cfr1_shadow, cfr_buf);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR1, cfr_buf, AD9910_REG_LEN_CFR1);
  if (status != AD9910_STATUS_OK)
  {
    dev->initialized = 0U;
    return status;
  }

  ad9910_pack_u32_be(dev->cfr2_shadow, cfr_buf);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR2, cfr_buf, AD9910_REG_LEN_CFR2);
  if (status != AD9910_STATUS_OK)
  {
    dev->initialized = 0U;
    return status;
  }

  status = AD9910_SetSysClockConfig(dev, &dev->cfg.sysclk, 0U);
  if (status != AD9910_STATUS_OK)
  {
    dev->initialized = 0U;
    return status;
  }

  status = AD9910_SetAuxDacFsc(dev, dev->aux_dac_fsc, 0U);
  if (status != AD9910_STATUS_OK)
  {
    dev->initialized = 0U;
    return status;
  }

  status = AD9910_IOUpdate(dev);
  if (status != AD9910_STATUS_OK)
  {
    dev->initialized = 0U;
    return status;
  }

  /* PLL 启用时等待锁定，避免后续寄存器操作在不稳定时钟下进行 */
  if (dev->cfg.sysclk.pll_enable != 0U)
  {
    ad9910_delay_cycles(40000U);
  }

  return AD9910_STATUS_OK;
}

/**
 * @brief 反初始化设备对象并清空内部状态。
 * @param dev 设备对象指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_DeInit(ad9910_t *dev)
{
  if (dev == NULL)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  memset(dev, 0, sizeof(*dev));
  return AD9910_STATUS_OK;
}

/**
 * @brief 触发 AD9910 主复位引脚。
 * @param dev 设备对象指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_Reset(ad9910_t *dev)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }

  AD9910_GPIO_SET(dev->cfg.reset.port, dev->cfg.reset.pin);
  ad9910_delay_cycles(128U);
  AD9910_GPIO_CLR(dev->cfg.reset.port, dev->cfg.reset.pin);
  ad9910_delay_cycles(128U);

  return AD9910_STATUS_OK;
}

/**
 * @brief 触发 IO_UPDATE 引脚，使寄存器改动生效。
 * @param dev 设备对象指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_IOUpdate(ad9910_t *dev)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }

  AD9910_GPIO_SET(dev->cfg.io_update.port, dev->cfg.io_update.pin);
  ad9910_delay_cycles(128U);
  AD9910_GPIO_CLR(dev->cfg.io_update.port, dev->cfg.io_update.pin);
  ad9910_delay_cycles(64U);

  return AD9910_STATUS_OK;
}

/**
 * @brief 触发 IO_RESET 引脚，复位串行接口状态机。
 * @param dev 设备对象指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_IOReset(ad9910_t *dev)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }

  if (ad9910_pin_valid(&dev->cfg.io_reset) == 0U)
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  AD9910_GPIO_SET(dev->cfg.io_reset.port, dev->cfg.io_reset.pin);
  ad9910_delay_cycles(64U);
  AD9910_GPIO_CLR(dev->cfg.io_reset.port, dev->cfg.io_reset.pin);
  ad9910_delay_cycles(64U);
  return AD9910_STATUS_OK;
}

/* ========================= 寄存器读写 ========================= */

/**
 * @brief 写 AD9910 指定寄存器。
 * @param dev 设备对象指针。
 * @param reg_addr 寄存器地址。
 * @param data 写入数据指针。
 * @param len 写入字节数。
 * @return 执行状态。
 */
ad9910_status_t AD9910_WriteRegister(ad9910_t *dev, uint8_t reg_addr, const uint8_t *data, uint16_t len)
{
  uint8_t instruction;
  uint16_t expect_len;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if (data == NULL)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  expect_len = ad9910_register_length(reg_addr);
  if ((expect_len == 0U) || (expect_len != len))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  instruction = AD9910_SPI_MAKE_INSTR(reg_addr, 0U);
  status = ad9910_cs_active(dev);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  status = ad9910_spi_tx(dev, &instruction, 1U);
  if (status == AD9910_STATUS_OK)
  {
    status = ad9910_spi_tx(dev, data, len);
  }

  (void)ad9910_cs_idle(dev);
  if (status == AD9910_STATUS_OK)
  {
    (void)ad9910_update_cached_register(dev, reg_addr, data);
  }
  return status;
}

#if (AD9910_ENABLE_READBACK == 1U)
/**
 * @brief 读 AD9910 指定寄存器。
 * @param dev 设备对象指针。
 * @param reg_addr 寄存器地址。
 * @param data 读回数据缓冲区指针。
 * @param len 读回字节数。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ReadRegister(ad9910_t *dev, uint8_t reg_addr, uint8_t *data, uint16_t len)
{
  uint8_t instruction;
  uint16_t expect_len;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if ((data == NULL) || (dev->cfg.enable_readback == 0U))
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  expect_len = ad9910_register_length(reg_addr);
  if ((expect_len == 0U) || (expect_len != len))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  instruction = AD9910_SPI_MAKE_INSTR(reg_addr, 1U);
  status = ad9910_cs_active(dev);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  status = ad9910_spi_tx(dev, &instruction, 1U);
  if (status == AD9910_STATUS_OK)
  {
    status = ad9910_spi_rx(dev, data, len);
  }
  (void)ad9910_cs_idle(dev);
  return status;
}
#endif

/* ========================= 时钟配置与换算 ========================= */

/**
 * @brief 配置系统时钟参数并更新 CFR3。
 * @param dev 设备对象指针。
 * @param cfg 时钟配置指针。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetSysClockConfig(ad9910_t *dev, const ad9910_sysclk_config_t *cfg, uint8_t auto_io_update)
{
  uint8_t buffer[4];
  uint32_t cfr3;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if ((cfg == NULL) || (cfg->sys_clk_hz == 0ULL) || (cfg->icp > 7U))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  /* PLL 启用时校验 VCO 选择与倍频系数范围 */
  if (cfg->pll_enable != 0U)
  {
    if ((uint8_t)cfg->vco_sel >= 6U)
    {
      return AD9910_STATUS_INVALID_PARAM;
    }
    if ((cfg->pll_multiplier < 12U) || (cfg->pll_multiplier > 127U))
    {
      return AD9910_STATUS_INVALID_PARAM;
    }
  }

  cfr3 = AD9910_CFR3_DEFAULT_VALUE;
  cfr3 &= ~(AD9910_CFR3_VCO_SEL_MASK |
    AD9910_CFR3_ICP_MASK |
    AD9910_BIT_U32(AD9910_CFR3_REFCLK_DIV_BYPASS_BIT) |
    AD9910_BIT_U32(AD9910_CFR3_REFCLK_DIV_RESETB_BIT) |
    AD9910_BIT_U32(AD9910_CFR3_PFD_RESET_BIT) |
    AD9910_BIT_U32(AD9910_CFR3_PLL_ENABLE_BIT) |
    AD9910_CFR3_PLL_N_MASK);

  cfr3 |= (((uint32_t)cfg->vco_sel & 0x7UL) << AD9910_CFR3_VCO_SEL_SHIFT);
  cfr3 |= (((uint32_t)cfg->icp & 0x7UL) << AD9910_CFR3_ICP_SHIFT);

  if (cfg->refclk_div_bypass != 0U)
  {
    cfr3 |= AD9910_BIT_U32(AD9910_CFR3_REFCLK_DIV_BYPASS_BIT);
  }
  if (cfg->refclk_div_resetb != 0U)
  {
    cfr3 |= AD9910_BIT_U32(AD9910_CFR3_REFCLK_DIV_RESETB_BIT);
  }
  if (cfg->pll_enable != 0U)
  {
    cfr3 |= AD9910_BIT_U32(AD9910_CFR3_PLL_ENABLE_BIT);
    cfr3 |= (((uint32_t)cfg->pll_multiplier & 0x7FUL) << AD9910_CFR3_PLL_N_SHIFT);
  }

  ad9910_pack_u32_be(cfr3, buffer);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR3, buffer, AD9910_REG_LEN_CFR3);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  dev->cfg.sysclk = *cfg;
  dev->cfr3_shadow = cfr3;

  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 读取缓存的系统时钟配置。
 * @param dev 设备对象指针。
 * @param cfg 输出时钟配置指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetSysClockConfig(const ad9910_t *dev, ad9910_sysclk_config_t *cfg)
{
  if ((dev == NULL) || (cfg == NULL) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  *cfg = dev->cfg.sysclk;
  return AD9910_STATUS_OK;
}

/**
 * @brief 将频率值换算为 FTW 字。
 * @param frequency_hz 频率值，单位 Hz。
 * @param sysclk_hz 系统时钟，单位 Hz。
 * @return FTW 值。
 */
uint32_t AD9910_FrequencyToFTW(double frequency_hz, double sysclk_hz)
{
  if ((frequency_hz <= 0.0) || (sysclk_hz <= 0.0))
  {
    return 0UL;
  }
  return ad9910_round_to_u32((frequency_hz * 4294967296.0) / sysclk_hz);
}

/**
 * @brief 将 FTW 字换算为频率值。
 * @param ftw FTW 值。
 * @param sysclk_hz 系统时钟，单位 Hz。
 * @return 频率值，单位 Hz。
 */
double AD9910_FTWToFrequency(uint32_t ftw, double sysclk_hz)
{
  if (sysclk_hz <= 0.0)
  {
    return 0.0;
  }
  return (((double)ftw) * sysclk_hz) / 4294967296.0;
}

/**
 * @brief 将相位角（度）换算为 POW 字。
 * @param phase_deg 相位角，单位度。
 * @return POW 值。
 */
uint16_t AD9910_PhaseDegToPOW(double phase_deg)
{
  while (phase_deg < 0.0)
  {
    phase_deg += 360.0;
  }
  while (phase_deg >= 360.0)
  {
    phase_deg -= 360.0;
  }
  return ad9910_round_to_u16((phase_deg * 65536.0) / 360.0);
}

/**
 * @brief 将 POW 字换算为相位角（度）。
 * @param pow_word POW 值。
 * @return 相位角，单位度。
 */
double AD9910_POWToPhaseDeg(uint16_t pow_word)
{
  return (((double)pow_word) * 360.0) / 65536.0;
}

/**
 * @brief 将归一化幅度换算为 ASF 字。
 * @param amplitude_scale 归一化幅度。
 * @return ASF 值。
 */
uint16_t AD9910_AmplitudeScaleToASF(double amplitude_scale)
{
  if (amplitude_scale <= 0.0)
  {
    return 0U;
  }
  if (amplitude_scale >= 1.0)
  {
    return AD9910_ASF_MAX;
  }
  return (uint16_t)(((double)AD9910_ASF_MAX * amplitude_scale) + 0.5);
}

/**
 * @brief 将 ASF 字换算为归一化幅度。
 * @param asf_word ASF 值。
 * @return 归一化幅度。
 */
double AD9910_ASFToAmplitudeScale(uint16_t asf_word)
{
  if (asf_word > AD9910_ASF_MAX)
  {
    asf_word = AD9910_ASF_MAX;
  }
  return ((double)asf_word) / ((double)(1UL << AD9910_ASF_BITS));
}

/* ========================= 单音模式接口 ========================= */

/**
 * @brief 设置 FTW 寄存器。
 * @param dev 设备对象指针。
 * @param ftw 目标 FTW 值。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetFTW(ad9910_t *dev, uint32_t ftw, uint8_t auto_io_update)
{
  uint8_t buffer[4];
  ad9910_status_t status;

  ad9910_pack_u32_be(ftw, buffer);
  status = AD9910_WriteRegister(dev, AD9910_REG_FTW, buffer, AD9910_REG_LEN_FTW);
  if ((status == AD9910_STATUS_OK) && (auto_io_update != 0U))
  {
    status = AD9910_IOUpdate(dev);
  }
  return status;
}

/**
 * @brief 设置 POW 寄存器。
 * @param dev 设备对象指针。
 * @param pow_word 目标 POW 值。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetPOW(ad9910_t *dev, uint16_t pow_word, uint8_t auto_io_update)
{
  uint8_t buffer[2];
  ad9910_status_t status;

  buffer[0] = (uint8_t)(pow_word >> 8);
  buffer[1] = (uint8_t)pow_word;

  status = AD9910_WriteRegister(dev, AD9910_REG_POW, buffer, AD9910_REG_LEN_POW);
  if ((status == AD9910_STATUS_OK) && (auto_io_update != 0U))
  {
    status = AD9910_IOUpdate(dev);
  }
  return status;
}

/**
 * @brief 设置 ASF 寄存器。
 * @param dev 设备对象指针。
 * @param asf_word 目标 ASF 值。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetASF(ad9910_t *dev, uint16_t asf_word, uint8_t auto_io_update)
{
  uint8_t buffer[4];
  uint32_t reg_value;
  ad9910_status_t status;

  if (asf_word > AD9910_ASF_MAX)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  reg_value = ((uint32_t)asf_word & AD9910_ASF_MAX) << 2U;
  ad9910_pack_u32_be(reg_value, buffer);

  status = AD9910_WriteRegister(dev, AD9910_REG_ASF, buffer, AD9910_REG_LEN_ASF);
  if ((status == AD9910_STATUS_OK) && (auto_io_update != 0U))
  {
    status = AD9910_IOUpdate(dev);
  }
  return status;
}

/**
 * @brief 一次性设置单音 FTW/POW/ASF 参数。
 *
 * 通过写入 Profile 0 寄存器实现。在单音模式下（OSK 禁用），DDS
 * 幅度由 Profile 中的 ASF 控制，而非独立的 ASF 寄存器（0x09）。
 *
 * @param dev 设备对象指针。
 * @param tone 单音参数结构体指针。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetSingleTone(ad9910_t *dev, const ad9910_single_tone_t *tone, uint8_t auto_io_update)
{
  if ((tone == NULL) || (tone->asf > AD9910_ASF_MAX))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  return AD9910_ProgramProfile(dev, AD9910_PROFILE_0,
    (const ad9910_profile_word_t *)tone, auto_io_update);
}

/**
 * @brief 以工程单位设置单音输出参数。
 * @param dev 设备对象指针。
 * @param frequency_hz 频率，单位 Hz。
 * @param phase_deg 相位，单位度。
 * @param amplitude_scale 归一化幅度。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetSingleToneHz(ad9910_t *dev,
  double frequency_hz,
  double phase_deg,
  double amplitude_scale,
  uint8_t auto_io_update)
{
  ad9910_single_tone_t tone;

  if ((dev == NULL) || (dev->cfg.sysclk.sys_clk_hz == 0ULL) ||
      (frequency_hz < 0.0) || (amplitude_scale < 0.0) || (amplitude_scale > 1.0))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  tone.ftw = AD9910_FrequencyToFTW(frequency_hz, (double)dev->cfg.sysclk.sys_clk_hz);
  tone.pow = AD9910_PhaseDegToPOW(phase_deg);
  tone.asf = AD9910_AmplitudeScaleToASF(amplitude_scale);

  return AD9910_SetSingleTone(dev, &tone, auto_io_update);
}

/**
 * @brief 读取当前单音参数（从 Profile 0 读取）。
 * @param dev 设备对象指针。
 * @param tone 输出单音参数指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetSingleTone(ad9910_t *dev, ad9910_single_tone_t *tone)
{
  if (tone == NULL)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

#if (AD9910_ENABLE_READBACK == 1U)
  return AD9910_ReadProfile(dev, AD9910_PROFILE_0, (ad9910_profile_word_t *)tone);
#else
  (void)dev;
  return AD9910_STATUS_NOT_SUPPORTED;
#endif
}

/* ========================= Profile 接口 ========================= */

/**
 * @brief 写入指定 Profile 的单音参数。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param word Profile 数据指针。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ProgramProfile(ad9910_t *dev,
  ad9910_profile_t profile,
  const ad9910_profile_word_t *word,
  uint8_t auto_io_update)
{
  uint64_t packed;
  uint8_t buffer[8];
  uint8_t reg_addr;
  ad9910_status_t status;

  if (((uint8_t)profile > (uint8_t)AD9910_PROFILE_7) || (word == NULL) || (word->asf > AD9910_ASF_MAX))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  packed = AD9910_PROFILE_PACK_SINGLE_TONE(word->ftw, word->pow, word->asf);
  ad9910_pack_u64_be(packed, buffer);
  reg_addr = (uint8_t)(AD9910_REG_PROFILE0 + (uint8_t)profile);

  status = AD9910_WriteRegister(dev, reg_addr, buffer, AD9910_REG_LEN_PROFILE);
  if ((status == AD9910_STATUS_OK) && (auto_io_update != 0U))
  {
    status = AD9910_IOUpdate(dev);
  }
  return status;
}

/**
 * @brief 读取指定 Profile 的单音参数。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param word 输出 Profile 数据指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ReadProfile(ad9910_t *dev, ad9910_profile_t profile, ad9910_profile_word_t *word)
{
  uint8_t buffer[8];
  uint64_t packed;
  ad9910_status_t status;

  if (((uint8_t)profile > (uint8_t)AD9910_PROFILE_7) || (word == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  status = AD9910_SelectProfilePins(dev, profile);
  if ((status != AD9910_STATUS_OK) && (status != AD9910_STATUS_NOT_SUPPORTED))
  {
    return status;
  }

#if (AD9910_ENABLE_READBACK == 1U)
  status = AD9910_ReadRegister(dev, (uint8_t)(AD9910_REG_PROFILE0 + (uint8_t)profile), buffer, AD9910_REG_LEN_PROFILE);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  packed = ad9910_unpack_u64_be(buffer);
  word->ftw = (uint32_t)(packed & 0xFFFFFFFFULL);
  word->pow = (uint16_t)((packed >> 32U) & 0xFFFFULL);
  word->asf = (uint16_t)((packed >> 48U) & AD9910_ASF_MAX);
  return AD9910_STATUS_OK;
#else
  (void)buffer;
  (void)packed;
  return AD9910_STATUS_NOT_SUPPORTED;
#endif
}

/**
 * @brief 根据 Profile 编号驱动 PROFILE[2:0] 引脚。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SelectProfilePins(ad9910_t *dev, ad9910_profile_t profile)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if ((uint8_t)profile > (uint8_t)AD9910_PROFILE_7)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  if (ad9910_profile_pins_valid(dev) == 0U)
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  if ((((uint8_t)profile) & 0x01U) != 0U)
  {
    AD9910_GPIO_SET(dev->cfg.profile0.port, dev->cfg.profile0.pin);
  }
  else
  {
    AD9910_GPIO_CLR(dev->cfg.profile0.port, dev->cfg.profile0.pin);
  }

  if ((((uint8_t)profile) & 0x02U) != 0U)
  {
    AD9910_GPIO_SET(dev->cfg.profile1.port, dev->cfg.profile1.pin);
  }
  else
  {
    AD9910_GPIO_CLR(dev->cfg.profile1.port, dev->cfg.profile1.pin);
  }

  if ((((uint8_t)profile) & 0x04U) != 0U)
  {
    AD9910_GPIO_SET(dev->cfg.profile2.port, dev->cfg.profile2.pin);
  }
  else
  {
    AD9910_GPIO_CLR(dev->cfg.profile2.port, dev->cfg.profile2.pin);
  }

  return AD9910_STATUS_OK;
}

/* ========================= UPDATE1 新增：DR/OSK 引脚接口 ========================= */

/**
 * @brief 设置 DR_HOLD 引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平，非 0 为高电平。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetDRHold(ad9910_t *dev, uint8_t level)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if (ad9910_pin_valid(&dev->cfg.dr_hold) == 0U)
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  if (level != 0U)
  {
    AD9910_GPIO_SET(dev->cfg.dr_hold.port, dev->cfg.dr_hold.pin);
  }
  else
  {
    AD9910_GPIO_CLR(dev->cfg.dr_hold.port, dev->cfg.dr_hold.pin);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 设置 DR_CTL 引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平，非 0 为高电平。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetDRCtl(ad9910_t *dev, uint8_t level)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if (ad9910_pin_valid(&dev->cfg.dr_ctl) == 0U)
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  if (level != 0U)
  {
    AD9910_GPIO_SET(dev->cfg.dr_ctl.port, dev->cfg.dr_ctl.pin);
  }
  else
  {
    AD9910_GPIO_CLR(dev->cfg.dr_ctl.port, dev->cfg.dr_ctl.pin);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 读取 DR_OVER 引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetDROver(ad9910_t *dev, uint8_t *level)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U) || (level == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }
  if (ad9910_pin_valid(&dev->cfg.dr_over) == 0U)
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  *level = (uint8_t)((dev->cfg.dr_over.port->IDR & dev->cfg.dr_over.pin) != 0U);
  return AD9910_STATUS_OK;
}

/**
 * @brief 设置 OSK 引脚电平。
 * @param dev 设备对象指针。
 * @param level 输出电平，非 0 为高电平。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetOSKPin(ad9910_t *dev, uint8_t level)
{
  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if (ad9910_pin_valid(&dev->cfg.osk) == 0U)
  {
    return AD9910_STATUS_NOT_SUPPORTED;
  }

  if (level != 0U)
  {
    AD9910_GPIO_SET(dev->cfg.osk.port, dev->cfg.osk.pin);
  }
  else
  {
    AD9910_GPIO_CLR(dev->cfg.osk.port, dev->cfg.osk.pin);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 配置手动 OSK 外部控制位。
 * @param dev 设备对象指针。
 * @param enable 非 0 使能手动 OSK，0 关闭。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ConfigureManualOSK(ad9910_t *dev, uint8_t enable, uint8_t auto_io_update)
{
  uint8_t buffer[4];
  uint32_t cfr1;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }

  cfr1 = dev->cfr1_shadow;
  cfr1 &= ~(AD9910_BIT_U32(AD9910_CFR1_MANUAL_OSK_EXT_CTRL_BIT) |
    AD9910_BIT_U32(AD9910_CFR1_OSK_ENABLE_BIT) |
    AD9910_BIT_U32(AD9910_CFR1_OSK_AUTO_BIT));

  if (enable != 0U)
  {
    cfr1 |= AD9910_BIT_U32(AD9910_CFR1_MANUAL_OSK_EXT_CTRL_BIT);
    cfr1 |= AD9910_BIT_U32(AD9910_CFR1_OSK_ENABLE_BIT);
  }

  ad9910_pack_u32_be(cfr1, buffer);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR1, buffer, AD9910_REG_LEN_CFR1);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  dev->cfr1_shadow = cfr1;

  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/* ========================= 辅助 DAC 与功耗控制 ========================= */

/**
 * @brief 设置辅助 DAC 的 FSC 控制码。
 * @param dev 设备对象指针。
 * @param fsc_code FSC 控制码。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetAuxDacFsc(ad9910_t *dev, uint8_t fsc_code, uint8_t auto_io_update)
{
  uint8_t buffer[4] = {0U, 0U, 0U, 0U};
  ad9910_status_t status;

  buffer[3] = fsc_code;
  status = AD9910_WriteRegister(dev, AD9910_REG_AUX_DAC, buffer, AD9910_REG_LEN_AUX_DAC);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  dev->aux_dac_fsc = fsc_code;
  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 获取缓存的辅助 DAC FSC 控制码。
 * @param dev 设备对象指针。
 * @param fsc_code 输出 FSC 控制码指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetAuxDacFsc(ad9910_t *dev, uint8_t *fsc_code)
{
  if ((dev == NULL) || (fsc_code == NULL) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  *fsc_code = dev->aux_dac_fsc;
  return AD9910_STATUS_OK;
}

/**
 * @brief 配置电源管理相关控制位。
 * @param dev 设备对象指针。
 * @param cfg 电源配置指针。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_SetPowerDown(ad9910_t *dev,
  const ad9910_power_down_config_t *cfg,
  uint8_t auto_io_update)
{
  uint32_t cfr1;
  uint8_t buffer[4];
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if (cfg == NULL)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  cfr1 = dev->cfr1_shadow;
  cfr1 &= ~(AD9910_BIT_U32(AD9910_CFR1_DIGITAL_POWER_DOWN_BIT) |
    AD9910_BIT_U32(AD9910_CFR1_DAC_POWER_DOWN_BIT) |
    AD9910_BIT_U32(AD9910_CFR1_REFCLK_POWER_DOWN_BIT) |
    AD9910_BIT_U32(AD9910_CFR1_AUX_DAC_POWER_DOWN_BIT) |
    AD9910_BIT_U32(AD9910_CFR1_EXT_POWER_DOWN_CTRL_BIT));

  if (cfg->digital_power_down != 0U)
  {
    cfr1 |= AD9910_BIT_U32(AD9910_CFR1_DIGITAL_POWER_DOWN_BIT);
  }
  if (cfg->dac_power_down != 0U)
  {
    cfr1 |= AD9910_BIT_U32(AD9910_CFR1_DAC_POWER_DOWN_BIT);
  }
  if (cfg->refclk_input_power_down != 0U)
  {
    cfr1 |= AD9910_BIT_U32(AD9910_CFR1_REFCLK_POWER_DOWN_BIT);
  }
  if (cfg->aux_dac_power_down != 0U)
  {
    cfr1 |= AD9910_BIT_U32(AD9910_CFR1_AUX_DAC_POWER_DOWN_BIT);
  }
  if (cfg->ext_power_down_ctrl != 0U)
  {
    cfr1 |= AD9910_BIT_U32(AD9910_CFR1_EXT_POWER_DOWN_CTRL_BIT);
  }

  ad9910_pack_u32_be(cfr1, buffer);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR1, buffer, AD9910_REG_LEN_CFR1);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  dev->cfr1_shadow = cfr1;
  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 获取当前电源管理控制位配置。
 * @param dev 设备对象指针。
 * @param cfg 输出电源配置指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GetPowerDown(ad9910_t *dev, ad9910_power_down_config_t *cfg)
{
  if ((dev == NULL) || (cfg == NULL) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  cfg->digital_power_down = (uint8_t)((dev->cfr1_shadow >> AD9910_CFR1_DIGITAL_POWER_DOWN_BIT) & 0x1U);
  cfg->dac_power_down = (uint8_t)((dev->cfr1_shadow >> AD9910_CFR1_DAC_POWER_DOWN_BIT) & 0x1U);
  cfg->refclk_input_power_down = (uint8_t)((dev->cfr1_shadow >> AD9910_CFR1_REFCLK_POWER_DOWN_BIT) & 0x1U);
  cfg->aux_dac_power_down = (uint8_t)((dev->cfr1_shadow >> AD9910_CFR1_AUX_DAC_POWER_DOWN_BIT) & 0x1U);
  cfg->ext_power_down_ctrl = (uint8_t)((dev->cfr1_shadow >> AD9910_CFR1_EXT_POWER_DOWN_CTRL_BIT) & 0x1U);
  return AD9910_STATUS_OK;
}

/* ========================= DRG 接口 ========================= */

/**
 * @brief 配置数字斜坡发生器（DRG）参数。
 * @param dev 设备对象指针。
 * @param cfg DRG 配置指针。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ConfigureDRG(ad9910_t *dev,
  const ad9910_drg_config_t *cfg,
  uint8_t auto_io_update)
{
  uint8_t buffer8[8];
  uint8_t buffer4[4];
  uint64_t limit_reg;
  uint64_t step_reg;
  uint32_t rate_reg;
  uint32_t cfr2;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if ((cfg == NULL) || ((uint8_t)cfg->destination > 2U))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  limit_reg = (((uint64_t)cfg->upper_limit) << 32U) | ((uint64_t)cfg->lower_limit);
  step_reg = (((uint64_t)cfg->step_down) << 32U) | ((uint64_t)cfg->step_up);
  rate_reg = (((uint32_t)cfg->rate_down) << 16U) | ((uint32_t)cfg->rate_up);

  ad9910_pack_u64_be(limit_reg, buffer8);
  status = AD9910_WriteRegister(dev, AD9910_REG_DIGITAL_RAMP_LIMIT, buffer8, AD9910_REG_LEN_DIGITAL_RAMP_LIMIT);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  ad9910_pack_u64_be(step_reg, buffer8);
  status = AD9910_WriteRegister(dev, AD9910_REG_DIGITAL_RAMP_STEP, buffer8, AD9910_REG_LEN_DIGITAL_RAMP_STEP);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  ad9910_pack_u32_be(rate_reg, buffer4);
  status = AD9910_WriteRegister(dev, AD9910_REG_DIGITAL_RAMP_RATE, buffer4, AD9910_REG_LEN_DIGITAL_RAMP_RATE);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  cfr2 = dev->cfr2_shadow;
  cfr2 &= ~(AD9910_CFR2_DRG_DEST_MASK |
    AD9910_BIT_U32(AD9910_CFR2_DRG_ENABLE_BIT) |
    AD9910_BIT_U32(AD9910_CFR2_DRG_NO_DWELL_HIGH_BIT) |
    AD9910_BIT_U32(AD9910_CFR2_DRG_NO_DWELL_LOW_BIT));
  cfr2 |= (((uint32_t)cfg->destination & 0x3UL) << AD9910_CFR2_DRG_DEST_SHIFT);

  if (cfg->enable != 0U)
  {
    cfr2 |= AD9910_BIT_U32(AD9910_CFR2_DRG_ENABLE_BIT);
  }
  if (cfg->no_dwell_high != 0U)
  {
    cfr2 |= AD9910_BIT_U32(AD9910_CFR2_DRG_NO_DWELL_HIGH_BIT);
  }
  if (cfg->no_dwell_low != 0U)
  {
    cfr2 |= AD9910_BIT_U32(AD9910_CFR2_DRG_NO_DWELL_LOW_BIT);
  }

  ad9910_pack_u32_be(cfr2, buffer4);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR2, buffer4, AD9910_REG_LEN_CFR2);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  dev->cfr2_shadow = cfr2;
  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 使能或关闭 DRG 功能。
 * @param dev 设备对象指针。
 * @param enable 非 0 使能，0 关闭。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_EnableDRG(ad9910_t *dev, uint8_t enable, uint8_t auto_io_update)
{
  uint32_t cfr2;
  uint8_t buffer[4];
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }

  cfr2 = dev->cfr2_shadow;
  if (enable != 0U)
  {
    cfr2 |= AD9910_BIT_U32(AD9910_CFR2_DRG_ENABLE_BIT);
  }
  else
  {
    cfr2 &= ~AD9910_BIT_U32(AD9910_CFR2_DRG_ENABLE_BIT);
  }

  ad9910_pack_u32_be(cfr2, buffer);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR2, buffer, AD9910_REG_LEN_CFR2);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  dev->cfr2_shadow = cfr2;
  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/* ========================= RAM 接口 ========================= */

/**
 * @brief 向 RAM 连续写入数据字。
 * @param dev 设备对象指针。
 * @param words 输入数据缓冲区指针。
 * @param word_count 写入字数。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_WriteRam(ad9910_t *dev,
  const uint32_t *words,
  uint16_t word_count,
  uint8_t auto_io_update)
{
  uint8_t buffer[4];
  uint16_t i;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if ((words == NULL) || (word_count == 0U) || (word_count > AD9910_RAM_DEPTH_WORDS))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  for (i = 0U; i < word_count; ++i)
  {
    ad9910_pack_u32_be(words[i], buffer);
    status = AD9910_WriteRegister(dev, AD9910_REG_RAM, buffer, AD9910_REG_LEN_RAM_WORD);
    if (status != AD9910_STATUS_OK)
    {
      return status;
    }
  }

  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

#if (AD9910_ENABLE_READBACK == 1U)
/**
 * @brief 从 RAM 连续读取数据字。
 * @param dev 设备对象指针。
 * @param words 输出数据缓冲区指针。
 * @param word_count 读取字数。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ReadRam(ad9910_t *dev,
  uint32_t *words,
  uint16_t word_count)
{
  uint8_t buffer[4];
  uint16_t i;
  ad9910_status_t status;

  if ((words == NULL) || (word_count == 0U) || (word_count > AD9910_RAM_DEPTH_WORDS))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  for (i = 0U; i < word_count; ++i)
  {
    status = AD9910_ReadRegister(dev, AD9910_REG_RAM, buffer, AD9910_REG_LEN_RAM_WORD);
    if (status != AD9910_STATUS_OK)
    {
      return status;
    }
    words[i] = ad9910_unpack_u32_be(buffer);
  }
  return AD9910_STATUS_OK;
}
#endif

/**
 * @brief 写入指定 Profile 的 RAM 播放控制字。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param cfg RAM Profile 配置指针。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ProgramRamProfile(ad9910_t *dev,
  ad9910_profile_t profile,
  const ad9910_ram_profile_config_t *cfg,
  uint8_t auto_io_update)
{
  uint64_t packed = 0ULL;
  uint8_t buffer[8];
  uint8_t reg_addr;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }

  if ((cfg == NULL) || ((uint8_t)profile > (uint8_t)AD9910_PROFILE_7) ||
      (cfg->start_addr > AD9910_RAM_ADDR_MAX) || (cfg->end_addr > AD9910_RAM_ADDR_MAX))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  packed |= (((uint64_t)cfg->step_rate) & 0xFFFFULL) << 40U;
  packed |= (((uint64_t)cfg->end_addr) & 0x03FFULL) << 30U;
  packed |= (((uint64_t)cfg->start_addr) & 0x03FFULL) << 14U;
  if (cfg->no_dwell_high != 0U)
  {
    packed |= (1ULL << 5U);
  }
  if (cfg->zero_crossing != 0U)
  {
    packed |= (1ULL << 3U);
  }
  packed |= ((uint64_t)cfg->mode & 0x7ULL);

  ad9910_pack_u64_be(packed, buffer);
  reg_addr = (uint8_t)(AD9910_REG_PROFILE0 + (uint8_t)profile);

  status = AD9910_WriteRegister(dev, reg_addr, buffer, AD9910_REG_LEN_PROFILE);
  if ((status == AD9910_STATUS_OK) && (auto_io_update != 0U))
  {
    status = AD9910_IOUpdate(dev);
  }
  return status;
}

/**
 * @brief 读取指定 Profile 的 RAM 播放控制字。
 * @param dev 设备对象指针。
 * @param profile Profile 编号。
 * @param cfg 输出 RAM Profile 配置指针。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ReadRamProfile(ad9910_t *dev,
  ad9910_profile_t profile,
  ad9910_ram_profile_config_t *cfg)
{
  uint8_t buffer[8];
  uint64_t packed;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }

  if (((uint8_t)profile > (uint8_t)AD9910_PROFILE_7) || (cfg == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  status = AD9910_SelectProfilePins(dev, profile);
  if ((status != AD9910_STATUS_OK) && (status != AD9910_STATUS_NOT_SUPPORTED))
  {
    return status;
  }

#if (AD9910_ENABLE_READBACK == 1U)
  status = AD9910_ReadRegister(dev, (uint8_t)(AD9910_REG_PROFILE0 + (uint8_t)profile), buffer, AD9910_REG_LEN_PROFILE);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  packed = ad9910_unpack_u64_be(buffer);
  cfg->step_rate = (uint16_t)((packed >> 40U) & 0xFFFFULL);
  cfg->end_addr = (uint16_t)((packed >> 30U) & 0x03FFULL);
  cfg->start_addr = (uint16_t)((packed >> 14U) & 0x03FFULL);
  cfg->no_dwell_high = (uint8_t)((packed >> 5U) & 0x1ULL);
  cfg->zero_crossing = (uint8_t)((packed >> 3U) & 0x1ULL);
  cfg->mode = (ad9910_ram_mode_t)(packed & 0x7ULL);
  return AD9910_STATUS_OK;
#else
  (void)buffer;
  (void)packed;
  return AD9910_STATUS_NOT_SUPPORTED;
#endif
}

/* ========================= UPDATE2：RAM 任意波形接口 ========================= */

/**
 * @brief 配置 RAM 功能开关与播放目标。
 * @param dev 设备对象指针。
 * @param enable 非 0 使能 RAM，0 关闭。
 * @param destination RAM 播放目标。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_ConfigureRamMode(ad9910_t *dev,
  uint8_t enable,
  ad9910_ram_destination_t destination,
  uint8_t auto_io_update)
{
  uint8_t buffer[4];
  uint32_t cfr1;
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if ((uint8_t)destination > 3U)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  cfr1 = dev->cfr1_shadow;
  cfr1 &= ~((1UL << 31U) | (3UL << 29U));

  if (enable != 0U)
  {
    cfr1 |= (1UL << 31U);
    cfr1 |= (((uint32_t)destination & 0x3UL) << 29U);
  }

  ad9910_pack_u32_be(cfr1, buffer);
  status = AD9910_WriteRegister(dev, AD9910_REG_CFR1, buffer, AD9910_REG_LEN_CFR1);
  if (status != AD9910_STATUS_OK)
  {
    return status;
  }

  dev->cfr1_shadow = cfr1;
  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 生成常用波形码表（1024 点，0~16383）。
 * @param cfg 波形配置指针。
 * @param out_codes 输出数组，长度 1024。
 * @return 执行状态。
 */
ad9910_status_t AD9910_GenerateWaveform(const ad9910_waveform_config_t *cfg,
  uint16_t out_codes[AD9910_RAM_POINT_COUNT])
{
  uint16_t i;
  uint16_t min_code;
  uint16_t max_code;
  uint32_t span;

  if ((cfg == NULL) || (out_codes == NULL))
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  min_code = cfg->min_code;
  max_code = cfg->max_code;
  if (min_code > AD9910_ASF_MAX)
  {
    min_code = AD9910_ASF_MAX;
  }
  if (max_code > AD9910_ASF_MAX)
  {
    max_code = AD9910_ASF_MAX;
  }
  if (max_code < min_code)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  span = (uint32_t)max_code - (uint32_t)min_code;

  switch (cfg->type)
  {
    case AD9910_WAVE_TRIANGLE:
      for (i = 0U; i < AD9910_RAM_POINT_COUNT; ++i)
      {
        uint32_t tri = (i < 512U) ? (i * 16383UL) / 511UL : ((1023UL - i) * 16383UL) / 511UL;
        uint32_t scaled = (span * tri) / 16383UL + (uint32_t)min_code;
        out_codes[i] = ad9910_clamp_code((uint16_t)scaled, min_code, max_code);
      }
      break;

    case AD9910_WAVE_SQUARE:
    {
      double duty = cfg->duty_cycle;
      uint16_t threshold;
      if ((duty <= 0.0) || (duty >= 1.0))
      {
        duty = 0.5;
      }
      threshold = (uint16_t)(duty * (double)AD9910_RAM_POINT_COUNT);
      if (threshold == 0U)
      {
        threshold = 1U;
      }

      for (i = 0U; i < AD9910_RAM_POINT_COUNT; ++i)
      {
        out_codes[i] = (i < threshold) ? max_code : min_code;
      }
      break;
    }

    case AD9910_WAVE_SINC:
    {
      double cycles = cfg->sinc_cycles;
      if (cycles <= 0.0)
      {
        cycles = 4.0;
      }

      for (i = 0U; i < AD9910_RAM_POINT_COUNT; ++i)
      {
        double x = (((double)i / (double)(AD9910_RAM_POINT_COUNT - 1U)) - 0.5) * (2.0 * AD9910_PI * cycles);
        double y;
        double n;
        uint16_t code;

        if ((x > -1e-12) && (x < 1e-12))
        {
          y = 1.0;
        }
        else
        {
          y = sin(x) / x;
        }

        n = (y + 1.0) * 0.5;
        if (n < 0.0)
        {
          n = 0.0;
        }
        if (n > 1.0)
        {
          n = 1.0;
        }

        code = (uint16_t)((double)min_code + n * (double)span + 0.5);
        out_codes[i] = ad9910_clamp_code(code, min_code, max_code);
      }
      break;
    }

    default:
      return AD9910_STATUS_INVALID_PARAM;
  }

  return AD9910_STATUS_OK;
}

/**
 * @brief 将 1024 点 14 位幅度码表装载到 AD9910 RAM。
 * @param dev 设备对象指针。
 * @param amp_codes 输入码表（1024 点）。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_LoadRamWaveform(ad9910_t *dev,
  const uint16_t amp_codes[AD9910_RAM_POINT_COUNT],
  uint8_t auto_io_update)
{
  uint16_t i;
  uint8_t bytes[4];
  ad9910_status_t status;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if (amp_codes == NULL)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  for (i = 0U; i < AD9910_RAM_POINT_COUNT; ++i)
  {
    uint32_t word = ad9910_pack_ram_amp_word(amp_codes[i]);
    ad9910_pack_u32_be(word, bytes);
    status = AD9910_WriteRegister(dev, AD9910_REG_RAM, bytes, AD9910_REG_LEN_RAM_WORD);
    if (status != AD9910_STATUS_OK)
    {
      return status;
    }
  }

  if (auto_io_update != 0U)
  {
    return AD9910_IOUpdate(dev);
  }
  return AD9910_STATUS_OK;
}

/**
 * @brief 一键生成并装载内置波形。
 * @param dev 设备对象指针。
 * @param cfg 波形生成配置。
 * @param auto_io_update 非 0 时自动触发 IO_UPDATE。
 * @return 执行状态。
 */
ad9910_status_t AD9910_LoadBuiltInWaveform(ad9910_t *dev,
  const ad9910_waveform_config_t *cfg,
  uint8_t auto_io_update)
{
  ad9910_status_t status;
  const uint16_t *table;

  if ((ad9910_device_valid(dev) == 0U) || (dev->initialized == 0U))
  {
    return AD9910_STATUS_NOT_INITIALIZED;
  }
  if (cfg == NULL)
  {
    return AD9910_STATUS_INVALID_PARAM;
  }

  status = ad9910_get_builtin_table(cfg->type, &table);
  if (status != AD9910_STATUS_OK)
  {
    uint16_t generated[AD9910_RAM_POINT_COUNT];
    status = AD9910_GenerateWaveform(cfg, generated);
    if (status != AD9910_STATUS_OK)
    {
      return status;
    }
    return AD9910_LoadRamWaveform(dev, generated, auto_io_update);
  }

  /* 内置波表默认满幅，若用户要求非满幅或自定义参数，回退到实时生成 */
  if ((cfg->min_code != 0U) || (cfg->max_code != AD9910_ASF_MAX) ||
      ((cfg->type == AD9910_WAVE_SQUARE) && (cfg->duty_cycle > 0.0) && (cfg->duty_cycle < 1.0) && ((cfg->duty_cycle < 0.499999) || (cfg->duty_cycle > 0.500001))) ||
      ((cfg->type == AD9910_WAVE_SINC) && (cfg->sinc_cycles > 0.0) && (cfg->sinc_cycles != 4.0)))
  {
    uint16_t generated2[AD9910_RAM_POINT_COUNT];
    status = AD9910_GenerateWaveform(cfg, generated2);
    if (status != AD9910_STATUS_OK)
    {
      return status;
    }
    return AD9910_LoadRamWaveform(dev, generated2, auto_io_update);
  }

  return AD9910_LoadRamWaveform(dev, table, auto_io_update);
}
