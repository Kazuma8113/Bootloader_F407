/**
  ******************************************************************************
  * @file    bootloader.h
  * @brief   A/B 双下载区 Bootloader：显式状态机 + 元数据日志 + 按键回滚。
  *
  *  核心思想：**永远不擦正在运行的那一份固件**。升级只写另一个下载区，
  *  写完整、CRC 校验通过，才把"活动区"标志切过去并给它打标记；
  *  任何一次升级失败都能按键回滚到上一个已验证的固件。
  ******************************************************************************
  */
#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "Flash.h"   /* A/B 下载区地址 + Flash 擦写 */
#include "Key.h"

/* ==================== 调试开关 ====================
 * 1 = 上电就进升级模式，不用按住 PG6（调试期方便反复烧固件用）。
 * 0 = 正常行为：只有按住 PG6 上电 / 元数据 ota_request=1 才进升级模式。
 *
 *  置 1 时会拦住"普通上电跳 APP"，只剩"刚验收完那一次"能跳（靠 just_committed
 *    放行），看着就像"写完固件重启后不跳转，反而一直提示写入X区"。发布前置 0。 */
#define BL_DEBUG_FORCE_OTA   0U


/* ==================== A/B 元数据日志 ====================
 * 存在 AB_LOG_ADDR（S9，128KB）里，**顺序追加**：每条 48 字节，写新记录不需要擦除
 * （F4 的擦除态是 0xFF，编程只能把 1 变成 0，往空白位置写就是合法的）。
 * 读的时候从最新位置往回找第一条"magic 正确 + 自校验 CRC 正确"的记录。
 *
 * 为什么不用"两页冗余 + 交替写"：一个扇区里擦第二页会连带擦掉第一页，冗余直接失效。
 * 追加日志则天然掉电安全 —— 掉电最多损坏最后那一条，它之前的记录全部完好
 * （self_crc 是结构体最后一个字段，它读得对就说明整条都写全了）。
 * 128KB / 48B ≈ 2730 条，升一次级才写 2~3 条。 */
#define AB_LOG_MAGIC     0x41424254UL   /* 'ABBT' */
#define AB_LOG_MAX       2048U          /* 2048 × 52 = 106496 字节 < 128KB，留余量 */

// 获取元数据日志指针
#define Get_log(ADDR) ((const AB_Log_t *) ADDR)

typedef struct {
    uint32_t magic;       /* == AB_LOG_MAGIC 才是有效记录 */
    uint32_t seq;         /* 单调递增；读时取数值最大的那条有效记录 */
    uint32_t active;      /* 当前活动区：0 = A，1 = B（上电跑这个区） */
    uint32_t pending;     /* 1 = 有一个新固件等着复位后被验收 */
    uint32_t target;      /* pending 时，新固件所在的那个区 */
    uint32_t ota_request; /* 1 = 请求进升级模式（按键回滚后 / APP 点升级 都会置它，一次性） */
    uint32_t attempts;    /* 本区已尝试启动次数（APP 确认启动成功后清零） */
    uint32_t crc[2];      /* 两区固件 CRC32；0xFFFFFFFF = 没登记过（Bootloader 没写过它） */
    uint32_t size[2];     /* 两区固件字节数。三态：
                           *   0xFFFFFFFF = 未登记（用 ST-Link 直烧的出厂固件，只查向量表）
                           *   0          = 明确不可用（正被升级改写 / 升级失败留下的半截）
                           *   其它       = 已登记，按 crc[] 全片校验 */
    uint32_t bl_id;       /* 写这条记录时 Bootloader 的构建指纹（见 BL_BuildId()）。
                           * 读日志只认指纹相同的记录 —— 换了 Bootloader 指纹就对不上，
                           * 旧的 A/B 状态整体作废，两个区都当空的，第一次升级写 A。 */
    uint32_t self_crc;    /* 上面所有字段的 CRC32（必须留在最后一个字段！） */
} AB_Log_t;               /* sizeof == 52：全是 uint32 无填充，可直接按字编程 */

/* 该区是否"已经被 Bootloader 验收过"（也就是"升级成功的标记"）。
 *  注意与 Slot_Verify() 的区别：这个只看标记，不做校验。 */
#define AB_SlotReady(m, idx)   ((m)->size[(idx)] != 0xFFFFFFFFUL && (m)->size[(idx)] != 0U)

/* attempts 达到此值就判定"这个区起不来"，自动回滚到另一个区。
 * 防的是"CRC 校验通过、但一跑就 HardFault" —— 这是 A/B 唯一测不出来的盲区，
 * 只能靠"跳过去 -> APP 跑起来后回报确认"闭环。
 *  目前 APP 侧还没有回报确认的逻辑，所以状态机里**暂不累加** attempts，
 *    等 APP 的确认接口一起上（否则每 3 次上电就会误判回滚）。 */
#define AB_MAX_ATTEMPTS  3U


/* ==================== 升级状态机的状态 ==================== */
typedef enum {
    /* ---- 启动阶段：选一个能跑的固件 ---- */
    ST_LOAD_META = 0,    /* 读元数据日志，并消费一次性的 ota_request        -> VERIFY_NEW | PICK_SLOT */
    ST_VERIFY_NEW,       /* 有 pending：校验新固件所在区                   -> COMMIT | DISCARD        */
    ST_COMMIT,           /* 验收通过：新固件成为活动区，并打上"可用"标记       -> PICK_SLOT              */
    ST_DISCARD,          /* 验收失败：丢弃新固件，旧区原封不动保留            -> PICK_SLOT              */
    ST_PICK_SLOT,        /* 挑一个可用的区（活动区优先，坏了就换另一个）       -> CHECK_KEY               */
    ST_CHECK_KEY,        /* 判断进升级还是跑固件（按住 PE6 / ota_request）   -> RUN_APP | NO_IMAGE | OTA_WAIT */
    ST_RUN_APP,          /* 跳转执行活动区固件（终结）                                                */
    ST_NO_IMAGE,         /* 两个区都没有可用固件，只能重新升级                -> OTA_INIT                */

    /* ---- 升级阶段：只动非活动区 ---- */
    ST_OTA_INIT,         /* 开串口/DMA，并提示本次要写哪个区                 -> OTA_WAIT                */
    ST_OTA_WAIT,         /* 等手机握手 + 下发 [OTA START]                   -> OTA_ERASE               */
    ST_OTA_ERASE,        /* 把目标区标成"不可用"，擦除它，回 [OTA START OK!]  -> OTA_RECV                */
    ST_OTA_RECV,         /* 逐包接收写入目标区                              -> OTA_COMMIT | OTA_WAIT   */
    ST_OTA_COMMIT,       /* 算 CRC32、登记 pending（打"待验收"标记）         -> (复位) | HALT          */

    ST_HALT              /* 元数据写不进去等硬件级故障：LED 快闪等人工介入（终结） */
} BootState_t;


void AB_Log_Read(AB_Log_t *out);


uint8_t AB_Log_Write(AB_Log_t *m);


uint8_t AB_Log_Rollback(void);

/* 软件 CRC32（IEEE 802.3 / zlib，多项式 0xEDB88320 反射）。
 * 从 crc=0 起算；要接着算就传上次的返回值。须与手机端/APP 端算法一致。 */
uint32_t Crc32(const uint8_t *p, uint32_t len, uint32_t crc);

/* 校验某个下载区里的固件是否可用（向量表必须落在本区内 + 已登记则查全片 CRC32）。 */
uint8_t Slot_Verify(uint32_t idx, const AB_Log_t *m);


uint32_t AB_TargetSlot(const AB_Log_t *m);

/* Bootloader 主入口：跑状态机。正常情况下不返回。 */
void Bootloader_Run(void);


#endif /* __BOOTLOADER_H */
