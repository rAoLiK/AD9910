/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ad9910.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static ad9910_t g_ad9910;

void DDS_Init(void) {
  ad9910_init_t init = AD9910_INIT_DEFAULT();

  init.sysclk.ref_clk_hz = 40000000ULL;
  init.sysclk.sys_clk_hz = 1000000000ULL;
  init.sysclk.pll_enable = 1U;
  init.sysclk.pll_multiplier = 25U;
  init.sysclk.vco_sel = AD9910_VCO_SEL_5;
  init.sysclk.icp = 1U;

  (void)AD9910_Init(&g_ad9910, &init);
}

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
  uint16_t asf_word = AD9910_AmplitudeScaleToASF(0.01);

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
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* USER CODE BEGIN 2 */
  DDS_Init();
  AD9910_SetSingleToneHz(&g_ad9910, 10000.0, 0.0, 1.0f, 1U);
  // DDS_ExpSweep_PhaseContinuous(1000, 500000.0, 1000, 10);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 144;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
