/**
  ******************************************************************************
  * @file    OTA.c
  * @brief   蓝牙 OTA 接收实现（逐包写入**备用槽**），含 JDY-33 波特率设置。
  *
 *  A/B 双下载区：本文件只负责"把固件收全并写进非活动区 + 登记到元数据日志"，
 *  **不判断也不执行跳转** —— 写完置 pending 后复位，由 Bootloader 的状态机统一
 *  走"校验 -> 切活动区 -> 跳转"。这样跳转逻辑只有一份，不会两边不一致。
 *
 *  本模块被拆成 4 个"状态机步骤函数"，由 bootloader.c 的 BootState_t 依次驱动：
 *      OTA_StepWaitStart() -> OTA_EraseTarget() -> OTA_ReceiveToSlot() -> OTA_MarkPending()
 *
 *  波特率策略（永久 115200）：
 *   - JDY-33 出厂默认 9600。上电时（蓝牙未连接、AT 命令才有效）发 "AT+BAUD8\r\n"
 *     把它改到 115200，之后保持不动（掉电保存）。
 *   - Bootloader / APP / 手机端全部使用 115200，不再来回切换。
 *   - 注意与 HC-08 的差异：HC-08 是 "AT+BAUD=115200"（不带换行），
 *     JDY-33 是序号式 "AT+BAUD8" 且**必须带 \r\n**，照抄旧命令模块不会理。
 ******************************************************************************
 */
#include "OTA.h"
#include "Flash.h"
#include "usart.h"
#include "Led.h"
#include "lcd_spi_154.h"
#include "Bluetooth.h"
#include "bootloader.h"   /* AB_Log_t / AB_Log_Read / AB_Log_Write / Crc32 */
#include <string.h>

static uint32_t s_wr_addr;   /* 当前写入地址（位于非活动区内） */
static uint32_t s_slot;      /* 本次升级的目标区（= 非活动区），0 = A，1 = B */


/* ==================== 收发 ==================== */
/* 等一帧（一次 IDLE 完成）。返回 1 = 收到一段。 */
static uint8_t OTA_WaitFrame(uint32_t timeout_ms) {
    uint32_t t0 = HAL_GetTick();
    while (RxEnd_Flag == 0) {
        if ((HAL_GetTick() - t0) >= timeout_ms) {
            return 0;
        }
    }
    RxEnd_Flag = 0;
    return 1;
}

/* 限时等待启动帧。到点仍未收到返回 0，由调用方决定是否继续等。 */
static uint8_t OTA_WaitStart(uint32_t timeout_ms) {
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms) {
        uint32_t left = timeout_ms - (HAL_GetTick() - t0);
        if (!OTA_WaitFrame(left)) break;

        if (strstr(Receive_Buf1, OTA_START_FRAME) != NULL) {
            return 1;
        }
        /* 手机握手帧：回 "[BOOTLOADER]"，让 APP 知道当前跑的是 Bootloader（升级模式）。
         * 回一次就够 —— APP 识别后即停止重发握手包。 */
        if (strstr(Receive_Buf1, OTA_CONN_FRAME) != NULL) {
            Bluetooth_Send(OTA_BOOTLOADER_REPLY);
        }
    }
    return 0;
}

/* ==================== 直读 DMA 环形缓冲 ==================== */

/* 从 USART1 的 DMA 环形缓冲里取"新到的"字节，返回取到的个数。
 *
 * 为什么不用 IDLE + Receive_Buf1：258 字节的数据包不可能保证落在单次 IDLE 突发里 ——
 * HC-08 时代是 BLE 把它拆成约 13 个 20 字节的包，每包到货都触发一次 IDLE；
 * 换 JDY-33 走 SPP 后，手机一次 write 就能整段发出，但 RFCOMM/L2CAP 在链路层
 * 仍可能把它切成多段投递（链路重传、流控、分片都会造成中途的空闲间隔）。
 * 只要中途出现一次 IDLE，ISR 就会 memset + 重填 Receive_Buf1，
 * 主函数正读到一半被 ISR 覆盖 → 中间字节永久丢失 → 一个包永远凑不满 → 卡死。
 * 直接读 DMA 写指针则没有共享缓冲，不存在这个竞争。
 * 前提：OTA_RxDirect=1（见 Usart1_IDLE_Callback）。 */
static uint16_t OTA_Pull(uint8_t *out, uint16_t max) {
    uint16_t n = 0;
    uint16_t cur = (uint16_t) (UART_BUFF_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx));

    while (RxBuff_Old != cur && n < max) {
        out[n++] = (uint8_t) Rx_Buf1[RxBuff_Old];
        RxBuff_Old = (uint16_t) ((RxBuff_Old + 1U) % UART_BUFF_SIZE);
    }
    return n;
}

/* 接收循环：逐字节切分，凑满 258 字节就写 Flash 并应答。
 * 返回 1 = 正常收到结束帧；0 = 手机久无数据（超时）。 */
/* 把无符号数以十进制追加到 buf[i] 位置，返回新的写下的位置（不写结束符）。 */
static int OTA_AppendNum(char *buf, int i, uint32_t v) {
    char digits[10];
    int d = 0;

    do {
        digits[d++] = (char) ('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (d > 0) buf[i++] = digits[--d];
    return i;
}

/* 回 "[OTA TIMEOUT RX:<n> OK:<a> SL:<s> ER:<e> ND:<d>]"：
 *   RX = 本次接收阶段累计收到的字节数
 *   OK = 258 字节窗口头尾校验通过（写 Flash + 回 ACK）的次数
 *   SL = 头尾校验失败、滑动 1 字节重对齐的次数
 *   ER = 本阶段 USART1 出错（ORE/FE/NE/PE）次数
 *   ND = 超时瞬间 DMA 剩余计数（1024 表示一个字节都没进环；其它值可与 RX 对照看指针是否卡死）
 * 判读：
 *   RX≈包长×包数、OK=0 → 字节都到了但窗口错位（链路丢/错字节）；
 *   RX 很小、ER 很大 → 每包都在中途被串口错误打断；
 *   RX 很小、ER=0、ND=1024 → DMA 根本没收到数据（模块没往 MCU 发）。
 * 手工转十进制，避免为了几个数字把 sprintf 家族（约 5KB）拖进 Bootloader。 */
static void OTA_ReplyTimeout(uint32_t rx, uint32_t ok, uint32_t slide,
                             uint32_t er, uint32_t nd) {
    char buf[80];
    int i = 0;
    const char *p;

    for (p = "[OTA TIMEOUT RX:"; *p != '\0'; p++) buf[i++] = *p;
    i = OTA_AppendNum(buf, i, rx);
    for (p = " OK:"; *p != '\0'; p++) buf[i++] = *p;
    i = OTA_AppendNum(buf, i, ok);
    for (p = " SL:"; *p != '\0'; p++) buf[i++] = *p;
    i = OTA_AppendNum(buf, i, slide);
    for (p = " ER:"; *p != '\0'; p++) buf[i++] = *p;
    i = OTA_AppendNum(buf, i, er);
    for (p = " ND:"; *p != '\0'; p++) buf[i++] = *p;
    i = OTA_AppendNum(buf, i, nd);
    buf[i++] = ']';
    buf[i] = '\0';
    Bluetooth_Send(buf);
}

/* 状态机步骤 3：逐包接收，写进 s_slot 指向的那个区。
 * 返回 1 = 正常收到 [OTA END]（固件收全）；0 = 手机久无数据（超时/断连）。 */
uint8_t OTA_ReceiveToSlot(void) {
    static uint8_t pkt[OTA_PKT_LEN];
    uint8_t ctl[OTA_END_LEN];       /* 攒 [OTA END] 控制帧用（防止被拆成多段） */
    uint16_t pkt_len = 0;
    uint16_t ctl_len = 0;
    uint32_t last = HAL_GetTick();
    uint32_t rx_total = 0;          /* 本次接收阶段累计收到的字节数（诊断用） */
    uint32_t ok_count = 0;          /* 窗口对齐成功次数（诊断用） */
    uint32_t slide_count = 0;       /* 窗口滑动重对齐次数（诊断用） */
    uint32_t err0 = Uart_ErrCount;  /* 本阶段起点，用来算"这一阶段出了几次错" */
    uint8_t b;

    for (;;) {
        if (OTA_Pull(&b, 1) == 0) {
            if ((HAL_GetTick() - last) >= OTA_WAIT_PKT_MS) {
                /* 手机久无数据：回带计数的超时（RX=总字节 OK=对齐次数 SL=滑动次数 ER=串口错误 ND=DMA余量） */
                OTA_ReplyTimeout(rx_total, ok_count, slide_count,
                                 Uart_ErrCount - err0,
                                 __HAL_DMA_GET_COUNTER(&hdma_usart1_rx));
                return 0;           /* 本次接收结束 */
            }
            continue;
        }
        rx_total++;
        last = HAL_GetTick();

        /* 包缓冲为空时，先尝试匹配 [OTA END] 控制帧 */
        if (pkt_len == 0) {
            if (b == (uint8_t) OTA_END_FRAME[ctl_len]) {
                ctl[ctl_len++] = b;
                if (ctl_len == OTA_END_LEN) {
                    Bluetooth_Send("[OTA END OK!]");
                    return 1;
                }
                continue;           /* 继续攒结束帧 */
            }
            /* 不是结束帧：把已攒的前缀当普通数据吐回，再处理本字节 */
            for (uint16_t k = 0; k < ctl_len && pkt_len < OTA_PKT_LEN; k++) {
                pkt[pkt_len++] = ctl[k];
            }
            ctl_len = 0;
        }

        pkt[pkt_len++] = b;

        if (pkt_len >= OTA_PKT_LEN) {
            if (pkt[0] == '[' && pkt[OTA_PKT_LEN - 1] == ']') {
                /* 只写括号中间的 256 字节数据，帧头帧尾都不进 Flash */
                uint32_t w = Flash_Write(s_wr_addr, &pkt[1], OTA_DATA_LEN);
                ok_count++;
                if (w == 0U) {
                    Bluetooth_Send("[DATA FRAMING ERROR!]");
                } else {
                    s_wr_addr += w;
                    Bluetooth_Send("[DATA SUCCESS!]");
                }
                pkt_len = 0;
            } else {
                /* 未对齐：丢 1 字节再试，直到窗口首尾对上 */
                slide_count++;
                memmove(pkt, pkt + 1, OTA_PKT_LEN - 1);
                pkt_len = OTA_PKT_LEN - 1;
            }
        }
    }
}


/* ==================== 状态机步骤（由 bootloader.c 驱动）==================== */

/* 步骤 0：初始化收发链路。状态机进入升级模式时调一次。 */
void OTA_StepInit(void) {
    Usart1_Init();                          /* 115200（波特率见 Core/Src/usart.c） */
    OTA_RxDirect = 0;                        /* 等启动帧阶段用 IDLE/Receive_Buf1 机制 */

    /* 调试用：模块还被留在工厂 9600 时，勾上这一行让 BL 自己把它改到 115200。
     * 调试时，不能连接蓝牙，否则 AT 指令无效
     * 平时用串口工具/手机 APP 配好一次即可（波特率掉电保存），这里保持注释。 */
    BLE_SetBaudTo115200();
}

/* 步骤 1：等手机握手 + 下发启动帧。每次调用只等一个 500ms 时间片，
 * 没消息就翻转 LED 并返回 0（让调用方的状态机可以继续跑别的判断）。
 * 返回 1 = 收到 [OTA START]，可以进入擦除步骤。
 *
 * 期间手机发握手帧 [SUCCESSFUL CONNECTION!] 时回 [BOOTLOADER]，
 * 让 APP 知道"当前跑的是 Bootloader（升级模式）"。 */
uint8_t OTA_StepWaitStart(void) {
    OTA_RxDirect = 0;                        /* 等启动帧阶段用 IDLE/Receive_Buf1 机制 */

    if (OTA_WaitStart(OTA_WAIT_SLICE_MS)) {
        return 1U;
    }
    LCD_ClearOneLine()
    LCD_DisplayText(0,100,"Bootloader 固件升级");
    LED_Flip(LED_PC13, 500);
    return 0U;
}

/* 步骤 2：擦除目标区（正在跑的那份固件全程一个字节都不动）。
 * 目标区由 AB_TargetSlot() 决定：常规是非活动区，出厂空板第一次强制 A。
 * 返回本次升级的目标区号（0 = A，1 = B）。 */
uint32_t OTA_EraseTarget(void) {
    AB_Log_t m;

    AB_Log_Read(&m);
    s_slot = AB_TargetSlot(&m);

    /* 先回"正在擦除"，让手机立刻停掉启动帧重发，
     * 否则擦除期间的重发会使 DMA 有残留，导致后续数据包永久错位。 */
    LCD_ClearOneLine()
    LCD_DisplayText(0,100,"正在擦除固件区");
    Bluetooth_Send(OTA_ERASE_REPLY);

    /* 擦除之前必须写入日志，告述这块区"明确不可用"
     * 防止擦到一半断电、或升级中途失败，回滚逻辑把这块区当成可运行的固件 */
    m.size[s_slot] = 0U;
    m.pending      = 0U;
    AB_Log_Write(&m);

    /* 擦 256KB = 2 × 128KB，约 2~3s */
    Flash_Erase(Flash_SlotSector(s_slot), SLOT_SECTORS);
    s_wr_addr = Flash_SlotAddr(s_slot);
    Usart1_FlushRx();                       /* 丢掉擦除期间手机发送的残留数据 */
    OTA_RxDirect = 1;                       /* 收发阶段由 OTA_Pull 直读 DMA，避开共享缓冲竞争 */

    LCD_ClearOneLine
    LCD_DisplayText(0,100,"固件区擦除完毕");
    Bluetooth_Send("[OTA START OK!]");
    return s_slot;
}

/* 步骤 3：固件收全后，算 CRC32 并登记进元数据日志：
 * pending=1 表示"复位后请验收这个区"。返回值 = 本次写入的字节数，0 表示失败。 */
uint32_t OTA_MarkPending(void) {
    AB_Log_t m;
    uint32_t sz = s_wr_addr - Flash_SlotAddr(s_slot);   /* 实际收到的字节数 */

    if (sz == 0U || sz > SLOT_SIZE) return 0U;

    LCD_ClearOneLine
    LCD_DisplayText(0,100,"正在校验固件");

    AB_Log_Read(&m);
    m.crc[s_slot]  = Crc32((const uint8_t *) Flash_SlotAddr(s_slot), sz, 0U);
    m.size[s_slot] = sz;
    m.pending      = 1U;
    m.target       = s_slot;
    m.attempts     = 0U;

    /* 元数据写不进去 = 硬件级故障 */
    if (!AB_Log_Write(&m)) return 0U;

    return sz;
}

