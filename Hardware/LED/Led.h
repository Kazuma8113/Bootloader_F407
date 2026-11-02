#ifndef GRADUATION_PROJECT_LED_H
#define GRADUATION_PROJECT_LED_H

#include "main.h"


extern GPIO_PinStruct LED_PC13;

void LED_Flip(GPIO_PinStruct GPIO, uint32_t Time);
void LED_SET(GPIO_PinStruct GPIO, GPIO_PinState state);

#endif //GRADUATION_PROJECT_LED_H
