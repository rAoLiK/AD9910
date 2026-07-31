#include "app_board.h"

#include "stm32f4xx_hal.h"

#define APP_RELAY_PORT             GPIOE
#define APP_RELAY_PIN              GPIO_PIN_5
#define APP_BUTTON_LINE_PORT       GPIOA
#define APP_BUTTON_LINE_PIN        GPIO_PIN_0
#define APP_BUTTON_CIRCLE_PORT     GPIOB
#define APP_BUTTON_CIRCLE_PIN      GPIO_PIN_9
#define APP_BUTTON_INFINITY_PORT   GPIOB
#define APP_BUTTON_INFINITY_PIN    GPIO_PIN_8
#define APP_BUTTON_IRQ_PRIORITY    (5U)

static volatile app_board_button_counts_t s_button_counts;
static volatile bool s_buttons_enabled;
static app_board_path_t s_path = APP_BOARD_PATH_DIRECT;

static GPIO_PinState AppBoard_RelayLevel(app_board_path_t path)
{
#if APP_RELAY_DIRECT_ACTIVE_HIGH
  return (path == APP_BOARD_PATH_DIRECT) ? GPIO_PIN_SET : GPIO_PIN_RESET;
#else
  return (path == APP_BOARD_PATH_DIRECT) ? GPIO_PIN_RESET : GPIO_PIN_SET;
#endif
}

void AppBoard_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();

  HAL_GPIO_WritePin(APP_RELAY_PORT, APP_RELAY_PIN,
                    AppBoard_RelayLevel(APP_BOARD_PATH_DIRECT));
  gpio.Pin = APP_RELAY_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(APP_RELAY_PORT, &gpio);

  gpio.Pin = APP_BUTTON_LINE_PIN;
  gpio.Mode = GPIO_MODE_IT_RISING;
  gpio.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(APP_BUTTON_LINE_PORT, &gpio);

  gpio.Pin = APP_BUTTON_CIRCLE_PIN | APP_BUTTON_INFINITY_PIN;
  gpio.Mode = GPIO_MODE_IT_FALLING;
  gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &gpio);

  HAL_NVIC_SetPriority(EXTI0_IRQn, APP_BUTTON_IRQ_PRIORITY, 0U);
  HAL_NVIC_SetPriority(EXTI9_5_IRQn, APP_BUTTON_IRQ_PRIORITY, 0U);
  AppBoard_EnableTask5Buttons(false);
  AppBoard_SetPath(APP_BOARD_PATH_DIRECT);
}

void AppBoard_SetPath(app_board_path_t path)
{
  if ((path != APP_BOARD_PATH_DIRECT) &&
      (path != APP_BOARD_PATH_DDS)) {
    path = APP_BOARD_PATH_DIRECT;
  }
  HAL_GPIO_WritePin(APP_RELAY_PORT, APP_RELAY_PIN,
                    AppBoard_RelayLevel(path));
  s_path = path;
}

app_board_path_t AppBoard_GetPath(void)
{
  return s_path;
}

void AppBoard_EnableTask5Buttons(bool enable)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  s_button_counts.line = 0U;
  s_button_counts.circle = 0U;
  s_button_counts.infinity = 0U;
  __HAL_GPIO_EXTI_CLEAR_IT(APP_BUTTON_LINE_PIN);
  __HAL_GPIO_EXTI_CLEAR_IT(APP_BUTTON_CIRCLE_PIN);
  __HAL_GPIO_EXTI_CLEAR_IT(APP_BUTTON_INFINITY_PIN);
  s_buttons_enabled = enable;
  if (enable) {
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
  } else {
    HAL_NVIC_DisableIRQ(EXTI0_IRQn);
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
  }
  __DMB();
  __set_PRIMASK(primask);
}

void AppBoard_TakeButtonCounts(app_board_button_counts_t *counts)
{
  uint32_t primask = __get_PRIMASK();

  if (counts == NULL) {
    return;
  }
  __disable_irq();
  counts->line = s_button_counts.line;
  counts->circle = s_button_counts.circle;
  counts->infinity = s_button_counts.infinity;
  s_button_counts.line = 0U;
  s_button_counts.circle = 0U;
  s_button_counts.infinity = 0U;
  __DMB();
  __set_PRIMASK(primask);
}

void HAL_GPIO_EXTI_Callback(uint16_t gpio_pin)
{
  if (!s_buttons_enabled) {
    return;
  }

  if ((gpio_pin == APP_BUTTON_LINE_PIN) &&
      (HAL_GPIO_ReadPin(APP_BUTTON_LINE_PORT,
                        APP_BUTTON_LINE_PIN) == GPIO_PIN_SET)) {
    if (s_button_counts.line != UINT8_MAX) {
      s_button_counts.line++;
    }
  } else if ((gpio_pin == APP_BUTTON_CIRCLE_PIN) &&
             (HAL_GPIO_ReadPin(APP_BUTTON_CIRCLE_PORT,
                               APP_BUTTON_CIRCLE_PIN) == GPIO_PIN_RESET)) {
    if (s_button_counts.circle != UINT8_MAX) {
      s_button_counts.circle++;
    }
  } else if ((gpio_pin == APP_BUTTON_INFINITY_PIN) &&
             (HAL_GPIO_ReadPin(APP_BUTTON_INFINITY_PORT,
                               APP_BUTTON_INFINITY_PIN) == GPIO_PIN_RESET)) {
    if (s_button_counts.infinity != UINT8_MAX) {
      s_button_counts.infinity++;
    }
  }
  __DMB();
}

void EXTI0_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(APP_BUTTON_LINE_PIN);
}

void EXTI9_5_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(APP_BUTTON_CIRCLE_PIN);
  HAL_GPIO_EXTI_IRQHandler(APP_BUTTON_INFINITY_PIN);
}
