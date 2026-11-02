/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.h
  * @brief   This file contains all the function prototypes for
  *          the usart.c file
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
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __USART_H__
#define __USART_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */
#include <string.h>
/* USER CODE END Includes */

extern UART_HandleTypeDef huart1;

/* USER CODE BEGIN Private defines */
/* DMA 环形接收缓冲。
 * ⚠️ 必须显著大于一个 OTA 数据包（258B）。原值 300 只有 1.16 包，
 *    手机上误重发/连续到达时环形缓冲立刻回绕覆盖，把还没取走的字节冲掉
 *    → 丢字节 → 只能靠 OTA_Receive 里 memmove 滑 1 字节慢慢重对齐（极慢）。
 *    1024 约等于 4 个包，给主循环留出处理余量。 */
#define UART_BUFF_SIZE 1024

extern volatile uint16_t RxBuff_Size, Receive_Buf_Size;
extern volatile uint8_t RxEnd_Flag, TxEnd_Flag;
extern volatile uint16_t RxBuff_New, RxBuff_Old;
/* USART1 出错次数（ORE/FE/NE/PE），用于诊断"只收到几十字节就停"的现场 */
extern volatile uint32_t Uart_ErrCount;
extern char Rx_Buf1[UART_BUFF_SIZE], Receive_Buf1[UART_BUFF_SIZE];
/* OTA 阶段直读 DMA 用：标志 + DMA 句柄 */
extern volatile uint8_t OTA_RxDirect;
extern DMA_HandleTypeDef hdma_usart1_rx;
/* USER CODE END Private defines */

void MX_USART1_UART_Init(void);

/* USER CODE BEGIN Prototypes */
void Usart1_Init(void);
void Usart1_IDLE_Callback(void);
void Usart1_FlushRx(void);
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __USART_H__ */

