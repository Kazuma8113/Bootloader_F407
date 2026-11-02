/**
  ******************************************************************************
  * @file    Flash.h
  * @brief   片内 Flash 读写 + A/B 双下载区分区表。
  ******************************************************************************
  */
#ifndef __FLASH_H
#define __FLASH_H

#include "stm32f4xx_hal.h"

/* ==================== A/B 双下载区分区表（1MB = Sector 0 ~ 11）====================
 *
 *   区名          起始地址        大小      扇区
 *   Bootloader    0x08000000     128KB      S0~S4   （S0-3 各 16KB + S4 64KB）
 *   下载区 A      0x08020000     256KB      S5~S6   （128KB × 2）
 *   下载区 B      0x08060000     256KB      S7~S8   （128KB × 2）
 *   元数据日志     0x080A0000     128KB      S9      （追加式写，见 AB_Log_t）
 *   保留          0x080C0000     256KB      S10~S11
 *
 * 升级流程（交替 A/B，永远不擦正在运行的那一份）：
 *   首次  -> 出厂固件烧在 A 区（或第一次 OTA 写 A），A 作为保底，绝不被动
 *   第 1 次升级 -> 写 B；B 校验通过 -> 给 B 打标记，活动槽切到 B
 *   第 2 次升级 -> 写 A；A 失败 -> 按键回滚到 B，重新握手再来
 *   如此交替。任何一次升级失败，另一个区的固件都毫发无损。
 *
 *  区必须由整数个扇区拼成，而 F407ZGT6 的扇区不是等大的：
 *    S0~S3 各 16KB、S4 是 64KB、S5~S11 各 128KB。
 *    所以 Bootloader 想凑到 128KB 就必须带上 S4；两个下载区只能从 S5 起铺。
 *    改大小时两个下载区的扇区数必须保持一致。
 *
 *  改这里的地址必须与 APP 工程**成对修改**：
 *      1) Graduation_F407/STM32F407ZGTX_FLASH.ld 的 FLASH ORIGIN / LENGTH
 *      2) Graduation_F407/Core/Src/system_stm32f4xx.c 的 VECT_TAB_OFFSET
 *    只改一边 = 向量表和链接地址不一致，跳过去必跑飞。
 * ============================================================================ */

#define BL_ADDR               0x08000000UL   /* Bootloader 区 */
#define BL_SIZE               0x00020000UL   /* 128KB（S0~S4） */

#define SLOT_A_ADDR           0x08020000UL   /* 下载区 A：S5~S6 */
#define SLOT_B_ADDR           0x08060000UL   /* 下载区 B：S7~S8 */
#define SLOT_SIZE             0x00040000UL   /* 每区 256KB */

#define SLOT_A_SECTOR         FLASH_SECTOR_5 /* 区首扇区号（擦除用，不是地址！） */
#define SLOT_B_SECTOR         FLASH_SECTOR_7
#define SLOT_SECTORS          2U             /* 每区占几个扇区（2 × 128KB = 256KB） */

#define AB_LOG_ADDR           0x080A0000UL   /* S9：元数据日志区 */
#define AB_LOG_SIZE           0x00020000UL   /* 128KB */
#define AB_LOG_SECTOR         FLASH_SECTOR_9
#define AB_LOG_SECTORS        1U             /* 日志区占 1 个扇区 */

#define SPARE_ADDR            0x080C0000UL   /* S10~S11：256KB 保留 */

/* 按区序号（0 = A，1 = B）取地址 / 首扇区号。 */
#define Flash_SlotAddr(idx)   ((idx) ? SLOT_B_ADDR : SLOT_A_ADDR)
#define Flash_SlotSector(idx) ((idx) ? SLOT_B_SECTOR : SLOT_A_SECTOR)
/* 另一个区的序号。 */
#define Flash_SlotOther(idx)  (1U - (idx))


uint8_t Flash_Erase(uint32_t first_sector, uint32_t sector_size);
uint32_t Flash_Write(uint32_t addr, const uint8_t *data, uint32_t len);
void Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len);

#endif /* __FLASH_H */
