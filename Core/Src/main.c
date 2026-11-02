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
#include "dma.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "bootloader.h"
#include "lcd_spi_154.h"


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
  MX_DMA_Init();
  MX_SPI3_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
    SPI_LCD_Init();
    LCD_SetTextFont(&CH_Font24);
  /* Bootloader：检查升级标志 -> 搬运 -> 跳转 APP（正常不返回，后面 while(1) 仅兜底） */
  Bootloader_Run();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
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
  RCC_OscInitStruct.PLL.PLLN = 168;
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

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* 外部中断回调：PG6 按下 -> 只置一个"请求弹菜单"的标志，立刻返回。
 * ⚠️ 真正的菜单处理放在主循环（Bootloader_Run 每轮调 Key_Menu_Process()）：
 *    本中断优先级 0，和 USART1/DMA 同级、互不抢占，在 ISR 里做长耗时的事会把串口
 *    和 DMA 全部挡住（OTA 收包直接丢），而且菜单里那个等按键的循环一旦出不来
 *    就是死机 —— 现场表现就是"按下直接死机"。 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == GPIO_PIN_6) {
        Key_Menu_Request();
    }
}

/* UART 发送中断回调函数 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    // 串口DMA发送完毕时调用
    if (huart->Instance == USART1) {
        TxEnd_Flag = 1;
    }
}

/* 串口DMA 缓存区溢出中断回调函数 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART1) {
        Uart_ErrCount++;                    /* 诊断计数：升级超时帧里会报出来 */
        /* 清除所有可能的错误标志 */
        // 当DMA没有及时搬运数据寄存器RDR中数据时
        __HAL_UART_CLEAR_OREFLAG(huart);    // 清除接收数据寄存器RDR过载错误标志位

        __HAL_UART_CLEAR_PEFLAG(huart);     // 清除奇偶校验错误标志位
        __HAL_UART_CLEAR_FEFLAG(huart);     // 清除停止位错误标志位
        __HAL_UART_CLEAR_NEFLAG(huart);     // 清除噪声错误标志位

        TxEnd_Flag = 1;                     // 重启串口发送完毕标志位

        /* 重开 DMA 接收。
         * ⚠️ 两个坑（升级时"只收到几十字节就再也不动了"的根因之一）：
         *  ① 出错时 HAL 已经把 DMA 停了，RxState 可能还停在 BUSY，
         *     这时直接 Receive_DMA 会返回 HAL_BUSY、接收永久停摆 —— 先 Abort 再开；
         *  ② DMA 重开后计数器回到满值（相当于读指针 cur=0），
         *     而 RxBuff_Old 还停在出错前的位置，OTA_Pull() 会一直判成"没有新数据"，
         *     手机怎么重发都收不到（现象：[OTA TIMEOUT RX:22] 之后一字节不进）。
         *     所以必须把影子读指针一起清零。 */
        if (huart->RxState != HAL_UART_STATE_READY) {
            HAL_UART_AbortReceive(huart);
        }
        HAL_UART_Receive_DMA(&huart1, (uint8_t *) Rx_Buf1, UART_BUFF_SIZE);
        RxBuff_Old = 0;
        RxBuff_New = 0;
    }
}
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
