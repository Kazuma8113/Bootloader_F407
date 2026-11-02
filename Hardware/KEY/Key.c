#include "Key.h"
#include "bootloader.h"
#include "lcd_spi_154.h"
#include "led.h"

/* ==================== PG6 人工干预按键（中断只置标志，处理放主循环）====================
 * 场景：升级失败 / 卡住时，按 PG6 弹出菜单，人工决定"取消"还是"回滚" */
GPIO_PinStruct Upgrade_Key = {GPIOG, GPIO_PIN_6};
GPIO_PinStruct Menu_Key = {GPIOG, GPIO_PIN_5};

/* PG6 的 EXTI 请求标志：ISR 里置 1。 */
volatile uint8_t key_menu_req = 0U;

/* "有没有得回滚" = 非活动槽是否已验证可用。*/
volatile uint8_t is_rollback_possible = 0U;

/* 引脚是否处于"按下"状态 */
uint8_t Key_IsPressed(GPIO_PinStruct GPIO) {
    return (uint8_t) (HAL_GPIO_ReadPin(GPIO.Port, GPIO.Pin) == (GPIO_PinState) KEY_ACTIVE_LEVEL);
}

/* 在 EXTI 中断上下文调用：只置标志、立刻返回。*/
void Key_Menu_Request(void) {
    // 防止AB两区都没写入，或没有版本可回滚时，进入升级界面
    if (is_rollback_possible == 0U) return;
    // Upgrade_Key 按下，升级回滚界面标志位置1
    key_menu_req = 1U;
}

/* 在主循环（非中断）上下文调用：没有请求时立刻返回，绝不打断状态机。 */
void Key_Menu_Process(void) {
    AB_Log_t OTA_log;
    uint32_t key_down_start_t;       // menu_key 按下开始时间
    uint8_t rollback_flag = 0U;

    if (key_menu_req == 0U) return;  /* 没有按键请求 */
    key_menu_req = 0U;               /* 初始化按键请求 */

    HAL_Delay(KEY_DEBOUNCE_MS);
    if (!Key_IsPressed(Upgrade_Key)) return;

    /* ---- 兜底再判一次"到底有没有得回滚" ---- */
    AB_Log_Read(&OTA_log);
    if (!Slot_Verify(Flash_SlotOther(OTA_log.active), &OTA_log)) {
        LCD_ClearRect(0, 48, 240, 128);
        LCD_SetColor(LCD_WHITE);
        LCD_DisplayText(72, 100, "不可回滚");
        HAL_Delay(1200);
        goto menu_exit;             /* 收起提示并把状态行补回去 */
    }

    /* ---- 弹菜单：占 y=48~176。OTA 的状态文字画在 y=100，退出时会补画回去 ---- */
    LCD_ClearRect(0, 48, 240, 128);
    LCD_SetColor(LCD_WHITE);
    LCD_DisplayText(48, 56, "是否回滚程序");
    LCD_DisplayText(48, 104, "短按PG5取消");
    LCD_DisplayText(24, 144, "长按PG5回滚升级");

    /* ---- 等 PG5 按下：LED 每 100ms 翻转一次当节拍（LED_Flip 自带阻塞延时，顺带起消抖作用）。
     * 必须带超时。这里原来是个**没有退出条件**的死循环，只认 PG5 按下：
     *    PG5 那边的按键没接/接触不良时，一进菜单就永远出不去 ——
     *    现场看到的就是"按下直接死机"。超时后按"短按取消"处理，正常收起菜单。 ---- */
    key_down_start_t = HAL_GetTick();    /* 【阶段一起点】进入菜单的时刻：超时收起的基准 */
    while (!Key_IsPressed(Menu_Key)) {
        if ((HAL_GetTick() - key_down_start_t) >= KEY_MENU_TIMEOUT_MS) {
            break;                  /* 超时 -> 当成"取消" */
        }
        LED_Flip(LED_PC13, 100);
    }

    /* ---- 真按下了 PG5 才继续判长按（超时退出的话直接跳到收尾） ---- */
    if (Key_IsPressed(Menu_Key)) {
        HAL_Delay(KEY_DEBOUNCE_MS);

        // 按键长按开始时间
        key_down_start_t = HAL_GetTick();
        while (Key_IsPressed(Menu_Key)) {
            if ((HAL_GetTick() - key_down_start_t) >= KEY_LONG_PRESS_MS) {
                rollback_flag = 1U;   /* 长按到门限 -> 回滚 */
                break;
            }
        }
    }

    /* ---- 长按：执行回滚 ---- */
    if (rollback_flag) {
        LCD_ClearRect(0, 48, 240, 128);
        LCD_DisplayText(48, 100, "正在回滚...");
        Usart1_FlushRx();       /* 丢弃按键期间堆在 DMA 环里的残留字节 */

        if (AB_Log_Rollback()) {
            /* 回滚成功：活动区已切回另一个区，并且置了一次性 ota_request。
             * 复位后状态机会停在"等启动帧"，手机重新握手就能重新升级。 */
            LCD_ClearRect(0, 48, 240, 128);
            LCD_DisplayText(48, 72, "已回滚");
            LCD_DisplayText(24, 130, "手机可重新升级");
            HAL_Delay(1200);
            NVIC_SystemReset();     /* 不返回 */
        }

        /* 回滚失败：另一个区也没有可用固件。提示后回到被打断的流程，
         * 让手机继续把这次升级做完（不要复位，否则又要从头来）。 */
        LCD_ClearRect(0, 48, 240, 128);
        LCD_DisplayText(48, 100, "回滚失败");
        HAL_Delay(1200);
    }

    menu_exit:
    /* ---- 收起菜单，回到被打断的流程继续（短按取消 / 等超时 / 不可回滚 都走这里）---- */
    LCD_ClearRect(0, 48, 240, 128);
    if (OTA_RxDirect) {
        /* 收包阶段的状态行不会自动重绘，手动补一条，免得屏幕空着 */
        LCD_DisplayText(0, 100, "正在接收固件...");
    } else {
        /* 等启动帧阶段：y=100 那行由状态机每 500ms 重绘，但"写入X区"提示
         * 只画一次，被菜单擦掉后得补回来，否则屏幕看着像卡住了。 */
        AB_Log_Read(&OTA_log);
        LCD_DisplayText(24, 130, (OTA_log.active == 0U) ? "写入B区" : "写入A区");
    }
    Usart1_FlushRx();               /* DMA 环可能已回绕，重新对齐读指针 */
}

