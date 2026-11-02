/**
  ******************************************************************************
  * @file    bootloader.c
  * @brief   A/B 双下载区 Bootloader：升级状态机 + 元数据日志 + 按键回滚。
  *
  *  分区与升级策略见 Flash.h / bootloader.h 的注释。一句话概括：
  *  **永远不擦正在运行的那一份固件** —— 升级只写另一个区，写完整、CRC 校验
  *  通过才切活动区并打标记；任何一次失败都能按键回滚到上一个已验证的固件。
  ******************************************************************************
  */
#include "bootloader.h"
#include "Led.h"
#include "OTA.h"
#include "Flash.h"
#include "lcd_spi_154.h"
#include "usart.h"

/* ==================== 软件 CRC32（IEEE 802.3 / zlib）====================
 * 多项式 0xEDB88320（反射），与 zlib.crc32 / 手机端保持一致。
 * 用 16 项半字节查表（每字节查两次）：比按位算法快约 4 倍，表只占 64 字节 Flash。
 * 自测向量：CRC32("123456789") == 0xCBF43926。 */
static const uint32_t s_crc_nib[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
    0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
    0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
};

/* CRC32 计算函数 */
uint32_t Crc32(const uint8_t *p, uint32_t len, uint32_t crc) {
    uint32_t i;

    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0FU];
        crc = (crc >> 4) ^ s_crc_nib[crc & 0x0FU];
    }
    return ~crc;
}


/* ==================== Bootloader 构建指纹 ====================
 * 用编译时间戳的 CRC32 当"这份 Bootloader 是谁"的指纹，写进每一条元数据记录；
 * 读日志时只认指纹相同的记录。于是"重新编译并烧录 Bootloader"= 指纹变了
 * = 旧的 A/B 状态整体作废 —— 两个下载区都当"空的"，第一次升级写 A、之后 A/B 交替。
 * 这就是"新烧 Bootloader 后默认从 A 区开始"的语义，不需要人为去清日志。
 *
 *  副作用：每次重新编译这个文件指纹都会变，只要重编重烧 A/B 状态就重置。
 *    调试时这正是想要的；正式发布若希望"重编也不重置"，换成手写版本宏即可
 *    （例如 `#define BL_BUILD_TAG "v1.0"`）。 */
static uint32_t BL_BuildId(void) {
    static const char tag[] = __DATE__ " " __TIME__;
    return Crc32((const uint8_t *) tag, sizeof(tag) - 1U, 0U);
}

/* ==================== A/B 元数据日志 ==================== */

/* 已写入的记录条数 = 从 0 数起第一个"空白位置"（magic 全 0xFF = 擦除态）的下标。
 * 中间被写坏一条也没关系，计数会继续往后走，读的时候靠 magic + self_crc 筛。 */
static uint32_t AB_Log_Count(void) {
    const AB_Log_t *p = Get_log(AB_LOG_ADDR);
    uint32_t n = 0U;

    while (n < AB_LOG_MAX && p[n].magic != 0xFFFFFFFFUL) {
        n++;
    }
    return n;
}

/* 读最新一条有效记录到 *out。日志全空/全坏时给安全默认值
 * （active = A，两个区都标"未登记"）。 */
void AB_Log_Read(AB_Log_t *out) {
    const AB_Log_t *p = Get_log(AB_LOG_ADDR);
    uint32_t cnt = AB_Log_Count();
    uint32_t n = cnt;
    uint32_t bl_id = BL_BuildId();

    /* 默认值：活动区 = A；两个区都标"未登记"（size = 0xFFFFFFFF），
     * 表示 Bootloader 没写过它们 —— 校验时只看向量表，不查 CRC。
     * 这样"只烧了 Bootloader + 用 ST-Link 直烧了一份固件"的出厂场景仍能跑。 */
    out->magic       = AB_LOG_MAGIC;
    out->seq         = 0U;
    out->active      = 0U;
    out->pending     = 0U;
    out->target      = 0U;
    out->ota_request = 0U;
    out->attempts    = 0U;
    out->crc[0]      = 0xFFFFFFFFUL;
    out->size[0]     = 0xFFFFFFFFUL;
    out->crc[1]      = 0xFFFFFFFFUL;
    out->size[1]     = 0xFFFFFFFFUL;
    out->bl_id       = bl_id;
    out->self_crc    = Crc32((const uint8_t *) out, sizeof(AB_Log_t) - 4U, 0U);

    /* 从最新位置往回找第一条"完整有效、且属于**当前这份 Bootloader**"的记录。
     * 掉电只可能损坏最后一条（写到一半），往回退一条就是掉电前的状态。 */
    while (n > 0U) {
        n--;
        if (p[n].magic == AB_LOG_MAGIC &&
            p[n].bl_id == bl_id &&
            Crc32((const uint8_t *) &p[n], sizeof(AB_Log_t) - 4U, 0U) == p[n].self_crc) {
            *out = p[n];
            return;
        }
    }

    /* 走到这里说明 S9 里**有旧记录、但没有一条属于当前这份 Bootloader**
     * —— 也就是"Bootloader 被重新编译烧录过"。
     * 把两个下载区都标成"明确不可用"，等价于"两区都是空的"：
     *   PICK_SLOT    -> have_slot = 0 -> NO_IMAGE -> 直接进升级模式（不用按 PG6）
     *   AB_TargetSlot-> 判"首次" -> 第一次升级写 A 区，之后 A/B 交替
     * 若 S9 一条记录都没有（全新板），则保持上面的默认值，让 ST-Link 直烧的
     * 出厂固件仍被信任并运行。 */
    if (cnt > 0U) {
        out->size[0] = 0U;
        out->size[1] = 0U;
    }
}
/* 追加一条记录（magic / seq / self_crc 由本函数填，seq = 上一条 + 1）。
 * 条数写满会先把整个 S9 扇区擦掉重来。失败返回 0。 */
uint8_t AB_Log_Write(AB_Log_t *m) {
    AB_Log_t prev;
    uint32_t n = AB_Log_Count();

    if (n >= AB_LOG_MAX) {
        /* 写满了才需要擦：整个 S9（128KB，约 2s）。2048 条 / 每次升级写 2~3 条
         * ≈ 一辈子碰不到几次。 擦的是 S9，当前代码在 S0~S4 里跑，安全。 */
        if (!Flash_Erase(AB_LOG_SECTOR, AB_LOG_SECTORS)) {
            return 0U;
        }
        n = 0U;
    }

    AB_Log_Read(&prev);                 /* 只为取上一条的 seq */
    m->magic    = AB_LOG_MAGIC;
    m->seq      = prev.seq + 1U;
    m->bl_id    = BL_BuildId();         /* 打上当前这份 Bootloader 的构建指纹 */
    m->self_crc = Crc32((const uint8_t *) m, sizeof(AB_Log_t) - 4U, 0U);

    /* 往空白位置追加，不需要擦除（F4 擦除态 0xFF，编程只能 1 -> 0）。
     * self_crc 是结构体最后一个字段，它读得对就说明整条都写全了。 */
    return (uint8_t) (Flash_Write(AB_LOG_ADDR + n * (uint32_t) sizeof(AB_Log_t),
                                  (const uint8_t *) m, (uint32_t) sizeof(AB_Log_t))
                      == (uint32_t) sizeof(AB_Log_t));
}

/* 按键回滚：把活动区切到**另一个**区，丢掉未验收的 pending，
 * 并置 ota_request=1 —— 这样复位后状态机会停在等启动帧，
 * 手机重新握手就能重新下发固件。
 * 返回 1 = 回滚成功；0 = 另一个区也没有可用固件，没得回滚。 */
uint8_t AB_Log_Rollback(void) {
    AB_Log_t m;
    uint32_t other;

    AB_Log_Read(&m);
    other = Flash_SlotOther(m.active);

    if (!Slot_Verify(other, &m)) {
        return 0U;                      /* 另一个区也没有可用固件，没得回滚 */
    }

    m.active      = other;              /* 活动区切回去 */
    m.pending     = 0U;                 /* 丢掉没通过验收的新固件 */
    m.target      = other;
    m.attempts    = 0U;
    /* 置一次性升级请求：复位后状态机会停在等启动帧，
     * 手机重新握手就能重新下发固件（这就是"回滚后重新开始升级"）。 */
    m.ota_request = 1U;
    return AB_Log_Write(&m);
}

/* ==================== 校验 与 跳转 ==================== */
/* 两区固件都不可用时同级的兜底：LED 快闪 + 死循环（一般走不到）。 */
static void Halt(void) {
    while (1) {
        LED_Flip(LED_PC13, 200);
    }
}

/* 校验某个下载区里的固件是否可用：向量表合法性 + （登记过就）全片 CRC32。
 *
 *  第一项检查顺便挡掉了"发错区的固件"：把链接在 A 区的 bin 写进 B 区时，
 *    它向量表里的 Reset_Handler 地址仍然指向 0x0802xxxx，不在
 *    [SLOT_B_ADDR, SLOT_B_ADDR + SLOT_SIZE) 内 -> 直接判非法，绝不会跳过去跑飞。
 *
 *  size 的三态语义（见 bootloader.h）：
 *    0xFFFFFFFF = 未登记：用 ST-Link 直烧的固件，只查向量表就放行
 *                （不放行的话首次烧录时日志还是空的，两个区都校验失败 -> 起不来）
 *    0          = 明确不可用：正在被升级改写 / 升级失败留下的半截 -> 一律拒绝
 *    其它       = 已登记：按 crc 全片校验
 *
 *  SP 上界必须**含端点**：F407 的 _estack = ORIGIN(RAM)+LENGTH(RAM) = 0x20020000，
 *    这是"RAM 末尾的下一个地址"，本身就是合法的初始栈顶；写成开区间会把正常程序判非法。 */
uint8_t Slot_Verify(uint32_t idx, const AB_Log_t *m) {
    uint32_t base = Flash_SlotAddr(idx);
    uint32_t sp = *(volatile uint32_t *) base;
    uint32_t pc = *(volatile uint32_t *) (base + 4U);

    uint8_t sp_ok = (sp >= 0x20000000UL && sp <= 0x20020000UL) ||    /* SRAM 128K，含栈顶端点 */
                    (sp >= 0x10000000UL && sp <= 0x10010000UL);      /* CCM 64K */
    uint8_t pc_ok = (pc >= base && pc < (base + SLOT_SIZE));

    if (!sp_ok || !pc_ok) {
        return 0U;                      /* 向量表不合法（空区 / 发错区 / 写坏） */
    }
    if (m->size[idx] == 0xFFFFFFFFUL) {
        return 1U;                      /* 未登记：信任直接烧录的固件 */
    }
    if (m->size[idx] == 0U || m->size[idx] > SLOT_SIZE) {
        return 0U;                      /* 明确不可用 / 记录里的长度不合法 */
    }
    return (uint8_t) (Crc32((const uint8_t *) base, m->size[idx], 0U) == m->crc[idx]);
}

/* 本次升级应该写哪个下载区：
 *   正常 = 非活动区（正在跑的那份固件一个字节都不动）
 *   特例 = 两个区都没有可用固件（出厂空板第一次 OTA）-> 强制写 A，
 *          让 A 成为"出厂保底固件"；A 从此永不升级失败，之后 A/B 才交替得起来。
 *          若这里仍按"非活动区"算，第一次会写进 B，A 永远空着，保底就没了。 */
uint32_t AB_TargetSlot(const AB_Log_t *m) {

    /*  出厂空板（两个区都没有可用固件）的第一次 OTA 必须写 A：
     *  这就是"最开始升级写入 A 区，A 区永远不会升级失败"的由来 ——
     *  A 一旦被验收就固定当保底，之后升级只在 B / A 之间交替。 */
    if (!Slot_Verify(0U, m) && !Slot_Verify(1U, m)) {
        return 0U;                      /* 首次：写 A */
    }
    return Flash_SlotOther(m->active);  /* 常规：写非活动区 */
}

/* 跳转到指定下载区的固件（永不返回）。 */
static void JumpToApp(uint32_t base) {
    uint32_t sp = *(volatile uint32_t *) base;
    uint32_t pc = *(volatile uint32_t *) (base + 4U);

    /* 复位外设与时钟，保证 APP 从干净状态启动（尤其关掉 SysTick）。 */
    HAL_RCC_DeInit();
    HAL_DeInit();
    __disable_irq();                // 关闭中断
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;     // 禁用所有 NVIC 中断
        NVIC->ICPR[i] = 0xFFFFFFFF;     // 清除所有 NVIC 挂起标志
    }

    /* 必须手动关 SysTick：HAL_RCC_DeInit() 末尾会调 HAL_InitTick() 把
     * SysTick 重新使能（此时系统时钟已回到 HSI 16MHz，中断周期约 95us）。
     * 不关掉的话，跳转后到 App 的 FreeRTOS 调度器接管之前，这个残留的
     * SysTick 中断会反复打进 App 的 SysTick_Handler / xPortSysTickHandler
     *（ uwTick 还没清零、内核状态未初始化），是经典的跳转雷区。 */
    SysTick->CTRL = 0;                  // 关闭 SysTick 定时器，避免残留中断
    SysTick->LOAD = 0;                  // 清除 SysTick 重装值
    SysTick->VAL  = 0;                  // 清除 SysTick 当前计数

    __set_MSP(sp);    /* 设 APP 的栈顶 */
    __set_CONTROL(0);        // 切换到 MSP 模式，避免仍在 PSP 下运行
    __ISB();                        // 指令同步屏障，确保寄存器修改立即生效
    SCB->VTOR = base;               /* 向量表切到目标区 */
    __enable_irq();                 // 重新打开中断，交给应用自己管理

    ((void (*)(void)) pc)();    /* 跳 APP 的 Reset_Handler */

    while (1);
}

/* ==================== 升级状态机 ====================
 * 状态转移图（"->" 表示下一个状态）：
 *
 *   LOAD_META ─┬─ pending=1 ─> VERIFY_NEW ─┬─ 校验过 ─> COMMIT  ─┐
 *              └─ pending=0 ─> PICK_SLOT    └─ 校验不过 ─> DISCARD ─┴─> PICK_SLOT
 *
 *   PICK_SLOT -> CHECK_KEY ─┬─ 有可用固件 且 没要求升级 ─> RUN_APP（跳转，终结）
 *                           ├─ 两个区都不可用            ─> NO_IMAGE ─┐
 *                           └─ 按住 PG6 / ota_request    ─> OTA_INIT ──┤
 *                                                                       ↓
 *   OTA_INIT -> OTA_WAIT -> OTA_ERASE -> OTA_RECV ─┬─ 收到 [OTA END] ─> OTA_COMMIT ─> 复位（终结）
 *                                                  └─ 超时/断连 ──────> OTA_WAIT   （重来）
 */
void Bootloader_Run(void) {
    AB_Log_t m;
    BootState_t st = ST_LOAD_META; // 默认初始状态
    uint8_t have_slot = 0U;     /* PICK_SLOT 的结果，供 CHECK_KEY 用 */
    uint8_t ota_req = 0U;       /* 本次上电是否被要求进升级模式 */
    uint8_t just_committed = 0U;/* 本次上电刚走完 ST_COMMIT（= 刚验收完一份新固件） */

    for (;;) {
        /* PG6 人工干预：EXTI 只置标志，真正的菜单在这里处理。
         * 放在状态机每轮开头 —— 每个状态转换点都能响应，又不会打断状态机内部逻辑。 */
        Key_Menu_Process();

        switch (st) {

        /* ---------- ① 读元数据日志 ---------- */
        case ST_LOAD_META:
            AB_Log_Read(&m);
            LCD_ClearRect(0, 48, 240, 128);

            /* ota_request 是**一次性**标志：读出来就清掉并写回，
             * 否则每次上电都会进升级模式，永远跑不了固件。 */
            ota_req = (uint8_t) (m.ota_request != 0U);
            if (m.ota_request) {
                m.ota_request = 0U;
                AB_Log_Write(&m);
            }
            st = m.pending ? ST_VERIFY_NEW : ST_PICK_SLOT;
            break;

        /* ---------- ② 有 pending：验收新固件 ---------- */
        case ST_VERIFY_NEW:
            st = Slot_Verify((m.target != 0U) ? 1U : 0U, &m) ? ST_COMMIT : ST_DISCARD;
            break;

        case ST_COMMIT:             /* 验收通过：新固件成为活动区，并打上"可用"标记 */
            m.active   = m.target;
            m.attempts = 0U;
            m.pending  = 0U;
            AB_Log_Write(&m);
            just_committed = 1U;    /* 标记"刚验收完"，防止调试开关又把这次塞回升级模式 */
            LCD_ClearRect(0, 48, 240, 128);
            LCD_DisplayText(24, 100, "固件升级成功");
            HAL_Delay(800);
            st = ST_PICK_SLOT;
            break;

        case ST_DISCARD:            /* 验收失败：丢掉新固件，旧区原封不动 */
            m.pending  = 0U;
            m.attempts = 0U;
            AB_Log_Write(&m);
            LCD_ClearRect(0, 48, 240, 128);
            LCD_DisplayText(24, 100, "固件升级失败");
            HAL_Delay(800);
            st = ST_PICK_SLOT;
            break;

        /* ---------- ③ 挑一个能跑的区 ---------- */
        case ST_PICK_SLOT: {
            uint8_t ok_a = Slot_Verify(0U, &m);
            uint8_t ok_b = Slot_Verify(1U, &m);
            uint8_t cur_ok = (m.active == 0U) ? ok_a : ok_b;

            if (cur_ok) {
                have_slot = 1U;
            } else if ((m.active == 0U) ? ok_b : ok_a) {
                m.active   = Flash_SlotOther(m.active);   /* 活动区坏了 -> 换另一个 */
                m.attempts = 0U;
                AB_Log_Write(&m);
                have_slot  = 1U;
            } else {
                have_slot = 0U;
            }

            /* 防止AB两区都没固件的时候进入按键回滚，导致死机 */
            is_rollback_possible = ((m.active == 0U) ? ok_b : ok_a) ? 1U : 0U;

            st = ST_CHECK_KEY;
            break;
        }

        /* ---------- ④ 进升级，还是跑固件 ---------- */
        case ST_CHECK_KEY: {
            uint8_t hold;

            /*  判断之前必须先关掉 PG6 的 EXTI：按住 PG6 上电会产生下降沿，
             *    不屏蔽的话 EXTI 会立刻把"是否回滚程序"菜单弹出来在里面死循环，
             *    和这里的启动判断打架。判断完再重新使能（升级等待期间还要用它）。 */
            HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);
            HAL_Delay(KEY_DEBOUNCE_MS);                     /* 等电平稳定 */
            hold = Key_IsPressed(Upgrade_Key);
            __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_6);           /* 清掉按住时挂起的边沿 */
            HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);

            if (!have_slot) {
                st = ST_NO_IMAGE;       /* 没有可跑的固件，只能重新升级 */
            } else if ((BL_DEBUG_FORCE_OTA && !just_committed) || hold || ota_req) {
                /* 进升级模式只认三个入口：调试开关（文件顶部 BL_DEBUG_FORCE_OTA）、
                 * 按住 PG6 上电（hold）、元数据 ota_request=1（按键回滚 / APP 点升级）。
                 *  调试开关必须与 !just_committed 相与：升级成功后复位回来会再走一遍
                 *    LOAD_META->VERIFY_NEW->COMMIT，开关=1 时若不放过这一次，
                 *    就会表现为"升级完成却不跳转"（被调试开关又塞回升级模式了）。 */
                st = ST_OTA_INIT;       /* 按住 PG6 上电 / 按键回滚后请求升级 */
            } else {
                st = ST_RUN_APP;
            }
            break;
        }

        /* ---------- ⑤ 跑固件（终结）---------- */
        case ST_RUN_APP: {
            uint32_t t0;

            LCD_ClearRect(0, 48, 240, 128);
            LCD_DisplayText(24, 100, m.active ? "运行SlotB固件" : "运行SlotA固件");

            /* 开机延时 2s，方便这段时间内按 PG6 回滚。
             *  不能直接写 HAL_Delay(2000)：PG6 中断现在只置标志，
             *    必须在这里轮询 Key_Menu_Process()，否则这 2s 内按键完全没反应。 */
            t0 = HAL_GetTick();
            while ((HAL_GetTick() - t0) < 2000U) {
                Key_Menu_Process();
            }
            HAL_Delay(1000);

            JumpToApp(Flash_SlotAddr(m.active));    /* 不返回 */
            st = ST_HALT;                           /* 兜底 */
            break;
        }

        /* ---------- ⑥ 两个区都没固件 ---------- */
        case ST_NO_IMAGE:
            LCD_ClearRect(0, 48, 240, 128);
            LCD_DisplayText(24, 100, "两槽固件不可用");
            HAL_Delay(1000);
            st = ST_OTA_INIT;
            break;

        /* ---------- ⑦ 升级模式：只动非活动区 ---------- */
        case ST_OTA_INIT:
            OTA_StepInit();
            /* 提示本次要写哪个区（常规 = 非活动区；空板第一次 = 强制 A） */
            LCD_DisplayText(24, 130, AB_TargetSlot(&m) ? "写入B区" : "写入A区");
            st = ST_OTA_WAIT;
            break;

        case ST_OTA_WAIT:
            if (OTA_StepWaitStart()) {
                st = ST_OTA_ERASE;
            }
            break;

        case ST_OTA_ERASE:
            OTA_EraseTarget();
            st = ST_OTA_RECV;
            break;

        case ST_OTA_RECV:
            if (OTA_ReceiveToSlot()) {
                st = ST_OTA_COMMIT;
            } else {
                /* 超时 / 断连：状态行提示一下，回等待重新来一遍。
                 * 非活动区被写了一半也无所谓 —— 它在元数据里已经是"明确不可用"，
                 * 活动区完好如初，绝不会变砖。 */
                LCD_ClearOneLine
                LCD_DisplayText(0, 100, "固件升级超时");
                HAL_Delay(1000);
                st = ST_OTA_WAIT;
            }
            break;

        case ST_OTA_COMMIT:
            if (OTA_MarkPending() == 0U) {
                LCD_ClearOneLine
                LCD_DisplayText(0, 100, "固件升级失败");
                st = ST_HALT;           /* 元数据写不进去 = 硬件级故障 */
                break;
            }
            LCD_ClearOneLine()
            LCD_DisplayText(0, 100, "固件升级成功");
            HAL_Delay(500);
            /* 复位 -> 回到 LOAD_META，读到自己刚写的 pending=1，
             * 走 VERIFY_NEW -> COMMIT 完成验收与切区。跳转逻辑只有一份。 */
            NVIC_SystemReset();
            st = ST_HALT;               /* 兜底 */
            break;

        /* ---------- 兜底 ---------- */
        case ST_HALT:
        default:
            Halt();
            break;
        }
    }
}
