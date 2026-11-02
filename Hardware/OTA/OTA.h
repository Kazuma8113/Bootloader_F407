/**
  ******************************************************************************
  * @file    OTA.h
  * @brief   蓝牙 OTA 接收：把手机端分包的固件逐包写入**非活动下载区**（A/B 交替）。
  *          协议：启动帧 [OTA START] -> 数据包 '['+256B+']'（258B）-> 结束帧 [OTA END]。
  *          每收一包应答 [DATA SUCCESS!] 或 [DATA FRAMING ERROR!]（停等，手机收到 SUCCESS 才发下一包）。
  *
  *  模块被拆成 4 个步骤函数，由 bootloader.c 里的 BootState_t 状态机依次驱动：
  *      OTA_StepInit()       仅一次：开串口 / DMA
  *      OTA_StepWaitStart()  等握手 + 启动帧，每次只等一个 500ms 时间片
  *      OTA_EraseTarget()    擦非活动区（先把它标成"不可用"再擦）
  *      OTA_ReceiveToSlot()  逐包接收，收全返回 1
  *      OTA_MarkPending()    算 CRC32 + 登记 pending（打"待验收"标记）
  *
  *  **本模块不跳转、不复位**，切活动区与跳转统一由 Bootloader 状态机完成。
  ******************************************************************************
  */
#ifndef __OTA_H
#define __OTA_H

#include "stm32f4xx_hal.h"

/* 数据包：'[' + 256 字节固件 + ']' = 258 字节 */
#define OTA_DATA_LEN        256
#define OTA_PKT_LEN         258

/* 启动 / 结束控制帧 */
#define OTA_START_FRAME     "[OTA START]"
#define OTA_END_FRAME       "[OTA END]"
#define OTA_END_LEN         (sizeof(OTA_END_FRAME) - 1U)   /* 结束帧字节数 */

/* 收到启动帧后、开始擦除非活动区前先回的应答：让手机立刻停掉启动帧重发。
 * 擦除要 2~3 秒，若不回这一条，手机期间重发的 [OTA START] 会残留在
 * 串口缓冲里，把后面所有数据包永久错位。 */
#define OTA_ERASE_REPLY     "[OTA ERASING!]"
/* 升级中久等不到数据时回一条，让手机知道是"MCU 收不到数据"，而不是无响应死机。 */
#define OTA_TIMEOUT_REPLY   "[OTA TIMEOUT]"

/* 手机连上后先发的握手帧；Bootloader 收到后回 OTA_BOOTLOADER_REPLY，
 * 让 APP 知道"当前跑的是 Bootloader（升级模式）"，而不是正常 APP。
 *  这个字符串 APP 端是用 contains() 子串匹配的，**不能随便加后缀**
 *    （比如改成 "[BOOTLOADER A]" 会让 APP 匹配不上，然后一直重发握手帧）。
 *    要报"当前活动区"，另开一条新帧。 */
#define OTA_CONN_FRAME       "[SUCCESSFUL CONNECTION!]"
#define OTA_BOOTLOADER_REPLY "[BOOTLOADER]"

/* 超时（ms） */
#define OTA_WAIT_SLICE_MS   500     /* 等待启动帧时每一轮的等待片长（片间翻转 LED） */

/*  升级中"等下一批字节"的最长时间（也是"手机已断连"的唯一判据）。
 *
 *  现在换 JDY-33 走经典蓝牙 SPP：一包 258B 一次 write，@115200 约 22ms 发完，
 *  整包往返（含写 Flash、回 [DATA SUCCESS!]）实测 50~100ms 量级，
 *
 *  取值 3000 有两个约束：
 *  ① 必须 大于 APP 端的单包重发间隔（OtaUploader.RESEND_MS = 2000），
 *     否则丢包时 APP 的重传还没到，MCU 就已经放弃接收了；
 *  ② 必须覆盖擦除残留（擦除 ~2.6s 期间手机可能还在发 [OTA START]）。
 *  满足 ① ② 且留了 30 倍于实测 RTT 的余量。 */
#define OTA_WAIT_PKT_MS     3000    /* 等下一包数据的最长时间（ms） */



/* ---- 状态机步骤函数 ---- */
void OTA_StepInit(void);            /* 开串口 + DMA（进升级模式时调一次） */
uint8_t OTA_StepWaitStart(void);    /* 1 = 收到 [OTA START] */
uint32_t OTA_EraseTarget(void);     /* 返回目标区号（0 = A，1 = B） */
uint8_t OTA_ReceiveToSlot(void);    /* 1 = 收到 [OTA END]，固件收全 */
uint32_t OTA_MarkPending(void);     /* 返回写入字节数，0 = 失败 */

#endif /* __OTA_H */
