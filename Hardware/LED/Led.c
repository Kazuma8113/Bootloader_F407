#include "Led.h"

GPIO_PinStruct LED_PC13 = {GPIOC, GPIO_PIN_13};

void LED_Flip(GPIO_PinStruct GPIO, uint32_t Time) {
    HAL_GPIO_TogglePin(GPIO.Port, GPIO.Pin);
    HAL_Delay(Time);
}

void LED_SET(GPIO_PinStruct GPIO, GPIO_PinState state) {
    HAL_GPIO_WritePin(GPIO.Port, GPIO.Pin, state);
}
