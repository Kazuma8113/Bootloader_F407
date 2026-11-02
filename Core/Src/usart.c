/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   This file provides code for the configuration
  *          of the USART instances.
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
#include "usart.h"

/* USER CODE BEGIN 0 */
volatile uint8_t RxEnd_Flag, TxEnd_Flag = 1;
volatile uint16_t RxBuff_New, RxBuff_Old;
volatile uint16_t RxBuff_Size, Receive_Buf_Size;
char Rx_Buf1[UART_BUFF_SIZE], Receive_Buf1[UART_BUFF_SIZE];
/* USART1 出错累计次数：只在诊断帧里上报，不参与控制 */
volatile uint32_t Uart_ErrCount = 0;

/* 1 = OTA 阶段：由 OTA.c 的 OTA_Pull() 直接读 DMA 环形缓冲，
 *     IDLE 回调不再参与 —— 避开"ISR 覆盖共享缓冲"导致丢字节的竞争。 */
volatile uint8_t OTA_RxDirect = 0;
/* USER CODE END 0 */

UART_HandleTypeDef huart1;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart1_tx;

/* USART1 init function */

void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspInit 0 */

  /* USER CODE END USART1_MspInit 0 */
    /* USART1 clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_9|GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* USART1 DMA Init */
    /* USART1_RX Init */
    hdma_usart1_rx.Instance = DMA2_Stream2;
    hdma_usart1_rx.Init.Channel = DMA_CHANNEL_4;
    hdma_usart1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_usart1_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart1_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart1_rx.Init.Mode = DMA_CIRCULAR;
    hdma_usart1_rx.Init.Priority = DMA_PRIORITY_LOW;
    hdma_usart1_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart1_rx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(uartHandle,hdmarx,hdma_usart1_rx);

    /* USART1_TX Init */
    hdma_usart1_tx.Instance = DMA2_Stream7;
    hdma_usart1_tx.Init.Channel = DMA_CHANNEL_4;
    hdma_usart1_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_usart1_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart1_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart1_tx.Init.Mode = DMA_NORMAL;
    hdma_usart1_tx.Init.Priority = DMA_PRIORITY_LOW;
    hdma_usart1_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart1_tx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(uartHandle,hdmatx,hdma_usart1_tx);

    /* USART1 interrupt Init */
    HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
  /* USER CODE BEGIN USART1_MspInit 1 */

  /* USER CODE END USART1_MspInit 1 */
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* uartHandle)
{

  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspDeInit 0 */

  /* USER CODE END USART1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9|GPIO_PIN_10);

    /* USART1 DMA DeInit */
    HAL_DMA_DeInit(uartHandle->hdmarx);
    HAL_DMA_DeInit(uartHandle->hdmatx);

    /* USART1 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART1_IRQn);
  /* USER CODE BEGIN USART1_MspDeInit 1 */

  /* USER CODE END USART1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */
void Usart1_Init(void) {
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);            // 使能空闲中断
    HAL_UART_Receive_DMA(&huart1, (uint8_t *) Rx_Buf1, UART_BUFF_SIZE); // 开启UART + DMA 数据接收
}

void Usart1_IDLE_Callback(void) {
    /* OTA 阶段直接用 OTA_Pull() 读 DMA，这里只清标志、不搬数据，
     * 否则 ISR 会在主函数读 Receive_Buf1 的过程中把它覆盖掉（丢字节）。 */
    if (OTA_RxDirect) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart1);
        return;
    }

    // UART 在接收端检测到至少 1 个字节时间没有新数据到来
    if ((__HAL_UART_GET_FLAG(&huart1, UART_FLAG_IDLE) != RESET)) { // 判断空闲中断标志位是否为0
        __HAL_UART_CLEAR_IDLEFLAG(&huart1); // 清除标志位

        // 总计数减去未传输的数据个数，得到已经接收的数据个数
        RxBuff_New = UART_BUFF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
        // 没有发送数据不进入判断，防止刚好接收满了时RxBuff_New为0，不处理数据
        if (RxBuff_New != RxBuff_Old) {
            RxEnd_Flag = 1; // 接受完成标志位置1

            // 复制数据
            memset(Receive_Buf1, 0, UART_BUFF_SIZE);   // 保证接收缓冲区数据干净
            if (RxBuff_New > RxBuff_Old) {
                RxBuff_Size = RxBuff_New - RxBuff_Old;
                memcpy(Receive_Buf1, &Rx_Buf1[RxBuff_Old], RxBuff_Size);
            } else { // 数据覆盖回转
                RxBuff_Size = UART_BUFF_SIZE - RxBuff_Old + RxBuff_New;
                memcpy(Receive_Buf1, &Rx_Buf1[RxBuff_Old], UART_BUFF_SIZE - RxBuff_Old);
                memcpy(&Receive_Buf1[UART_BUFF_SIZE - RxBuff_Old], Rx_Buf1, RxBuff_New);
            }
            RxBuff_Old = RxBuff_New;
            Receive_Buf_Size = RxBuff_Size;
        }
    }
}

/* 丢弃"已收到但尚未处理"的字节：把读指针对齐到 DMA 当前写位置。
 * 用途：OTA 进入接收阶段前，清掉擦除 APP 区期间手机重发进来的 [OTA START]，
 * 否则这些残留字节会把后面的数据包全部错位（表现为一直 [DATA FRAMING ERROR!]）。 */
void Usart1_FlushRx(void) {
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    RxBuff_New = UART_BUFF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
    RxBuff_Old = RxBuff_New;      /* 两者相等 → 已收字节视为"已消费" */
    RxEnd_Flag = 0;
    Receive_Buf_Size = 0;
}
/* USER CODE END 1 */
