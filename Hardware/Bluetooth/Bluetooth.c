#include "Bluetooth.h"
#include <string.h>

/* 通过蓝牙发送一段数据（DMA）。 */
void Bluetooth_SendHandler(uint8_t *Buff, uint16_t len) {
    /* 使用静态存储，防止函数返回后 DMA 读到失效数据 */
    static uint8_t tx_buf[UART_BUFF_SIZE];
    uint32_t start;

    if (len == 0 || len >= UART_BUFF_SIZE) return;

    /* 上一次发送还没完成时短暂等待，而不是直接丢弃。*/
    start = HAL_GetTick();
    while (TxEnd_Flag == 0) {
        if ((HAL_GetTick() - start) > BLE_WAIT_SEND) return;   /* 超时放弃，防止卡死 */
    }

    TxEnd_Flag = 0;
    memcpy(tx_buf, Buff, len);
    if (HAL_UART_Transmit_DMA(&huart1, tx_buf, len) != HAL_OK) {
        TxEnd_Flag = 1;   /* 启动失败就恢复标志，避免之后再也发不出去 */
    }
}

void Bluetooth_Send(const char *s) {
    /* ACK 末尾带换行：手机端按行拆包时能立刻识别，不必等 400ms 空闲超时 */
    HAL_UART_Transmit(&huart1, (uint8_t *) s, (uint16_t) strlen(s), 100);
    HAL_UART_Transmit(&huart1, (uint8_t *) "\n", 1, 100);
}

/* ==================== JDY-33 控制 ==================== */

/* 发 AT 命令（JDY-33 的 AT 指令必须自带 \r\n，命令串里已经带上） */
void BLE_SendAT(const char *cmd) {
    HAL_UART_Transmit(&huart1, (uint8_t *) cmd, (uint16_t) strlen(cmd), 100);
}

/* 切换 MCU 的 USART1 波特率并重开 DMA 接收 */
void Usart_SetBaud(uint32_t baud) {
    HAL_UART_DeInit(&huart1);
    huart1.Init.BaudRate = baud;
    if (HAL_UART_Init(&huart1) != HAL_OK) {
        Error_Handler();
    }
    RxEnd_Flag = 0;
    Usart1_Init();
}

/* 把 JDY-33 从出厂 9600 改到 115200，并关掉串口状态日志。
 * 波特率与 ENLOG 都是掉电保存，只需成功一次；平时不用调（默认注释）。
 * ⚠️ 两个条件必须满足，否则模块不回 +OK：
 *    ① 模块**未连接**（已被手机/电脑连着时 AT 指令无效，先 AT+DISC 或断电）；
 *    ② 波特率对得上（出厂 9600，若上次已改成 115200 则这里要先对齐 115200）。*/
void BLE_SetBaudTo115200(void) {
    Usart_SetBaud(9600);                    /* 先对齐出厂波特率 */
    HAL_Delay(500);                         /* 等模块上电就绪 */
    BLE_SendAT(BLE_DISABLE_LOG_CMD);        /* 先关状态日志（此时还是 9600） */
    HAL_Delay(200);
    BLE_SendAT(BLE_SET_BAUD_CMD);           /* AT+BAUD8：模块立即切到 115200 */
    HAL_Delay(300);
    Usart_SetBaud(115200);                 /* MCU 跟到 115200 */
}
