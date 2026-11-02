#include "Flash.h"
#include <string.h>

/* 擦除从 first_sector 开始的 sector_size 个扇区，成功返回 1 */
uint8_t Flash_Erase(uint32_t first_sector, uint32_t sector_size) {
    FLASH_EraseInitTypeDef erase;
    uint32_t err = 0;

    HAL_FLASH_Unlock();
    // 清除所有Flash错误状态标志
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;  // 按扇区擦除
    erase.Sector = first_sector;                // 起始扇区号
    erase.NbSectors = sector_size;              // 扇区数量
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &err) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0;
    }
    HAL_FLASH_Lock();
    return 1;
}

/* Flash 写入 data 到 addr，返回实际占用字节数
 * 返回本次实际占用且已 4 字节对齐的字节数 */
uint32_t Flash_Write(uint32_t addr, const uint8_t *data, uint32_t len) {
    uint32_t words, i, j;

    /* 无数据 或 目标地址未 4 字节对齐 */
    if (len == 0U || (addr & 0x3U) != 0U) return 0U;

    words = (len + 3U) / 4U;                /* 向上取整到整字 */
    HAL_FLASH_Unlock();
    for (i = 0; i < words; i++) {
        uint32_t word = 0xFFFFFFFFUL;       /* 缺的字节默认 0xFF（= Flash 擦除态） */
        uint32_t remain = len - i * 4U;
        uint8_t *w = (uint8_t *) &word;

        for (j = 0; j < remain && j < 4U; j++) {
            w[j] = data[i * 4U + j];
        }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i * 4U, word) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0U;
        }
    }
    HAL_FLASH_Lock();
    return words * 4U;                      /* 返回已补齐到 4 字节的占用长度 */
}

/* 从片内 Flash 读 len 字节到 buf。片内 Flash 本来就能按地址直接读 */
void Flash_Read(uint32_t addr, uint8_t *buf, uint32_t len) {
    memcpy(buf, (const void *) addr, len);
}
