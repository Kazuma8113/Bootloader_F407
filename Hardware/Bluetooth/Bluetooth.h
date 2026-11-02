/**
  ******************************************************************************
  * @file    Bluetooth.h
  * @brief   蓝牙（JDY-33，经典蓝牙 SPP 透传）发送接口。
  *          Bootloader 只保留发送函数；固件接收由 OTA 模块负责。
  ******************************************************************************
  */
#ifndef __BLUETOOTH_H__
#define __BLUETOOTH_H__

#include "main.h"
#include "usart.h"


/* 设置波特率为 115200：
 *   JDY-33 用「序号」作参数，8 = 115200（4=9600 … A=230400 … E=921600），
 *   ⚠️ AT 指令**必须带 \r\n**，否则模块完全不理；
 *   这一点和 HC-08 的 "AT+BAUD=115200"（不带换行）不同，是换型最容易踩的坑。 */
#define BLE_SET_BAUD_CMD    "AT+BAUD8\r\n"
/* 关闭串口状态日志：JDY-33 出厂 AT+ENLOG=1，会把连接/断开状态字节直接吐到 TXD，
 * 这些非协议字节会污染 MCU 侧的 "[...]" 帧解析，所以要关掉。 */
#define BLE_DISABLE_LOG_CMD "AT+ENLOG0\r\n"
/* 等待蓝牙发送完成时间，单位：ms */
#define BLE_WAIT_SEND       100U

/* 通过蓝牙发送一段数据（DMA，异步）。 */
void Bluetooth_SendHandler(uint8_t *Buff, uint16_t len);
void Bluetooth_Send(const char *s);
void BLE_SendAT(const char *cmd);
void Usart_SetBaud(uint32_t baud);
void BLE_SetBaudTo115200(void);

#endif /* __BLUETOOTH_H__ */
