#ifndef __KEY_H__
#define __KEY_H__

#include "main.h"


/* ==================== PG6 人工干预按键 ====================
 * 引脚配置：（按键一端接地，按下 = 低电平 -> 下降沿；写成 IT_RISING 就变成松手才触发）
 *   PG6 -> EXTI6，下降沿触发（GPIO_MODE_IT_FALLING），内部上拉
 *   PG5 -> 普通输入，内部上拉（同一接法），菜单里"取消 / 长按回滚"用它 */
extern GPIO_PinStruct Upgrade_Key;  // PG6
extern GPIO_PinStruct Menu_Key;     // PG5
extern volatile uint8_t key_menu_req;
extern volatile uint8_t is_rollback_possible;

#define KEY_ACTIVE_LEVEL    0U      /* 按键按下时引脚电平：0 = 低电平有效（接地） */
#define KEY_DEBOUNCE_MS     20U     /* 消抖时间（ms） */
#define KEY_LONG_PRESS_MS   1000U   /* 长按判定门限（ms）：< 门限为短按，>= 为长按 */
#define KEY_MENU_TIMEOUT_MS 10000U  /* 菜单无人操作自动收起的时间（ms）。*/

/* 中断只置标志、处理放主循环（原因见 bootloader.c 顶部说明）：
 *   Key_Menu_Request() —— 在 PG6 的 EXTI 中断里调，只置一个标志，立刻返回；
 *   Key_Menu_Process() —— 在 Bootloader_Run() 主循环里调，有请求才弹菜单 */

void Key_Menu_Request(void);
void Key_Menu_Process(void);

// 按键是否被按下
uint8_t Key_IsPressed(GPIO_PinStruct GPIO);
#endif //BOOTLOADER_F407_KEY_H

