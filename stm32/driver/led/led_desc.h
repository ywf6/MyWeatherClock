/* [阅读说明]
 * LED 描述符结构体定义：描述一个 LED 的完整硬件配置。
 * 包含 GPIO 端口/引脚和开/关时对应的电平值（支持高有效和低有效）。
 */
#ifndef __LED_DESC_H__
#define __LED_DESC_H__

#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx.h"

/* LED 描述符结构体 */
struct led_desc
{
    GPIO_TypeDef* Port;     /* GPIO 端口（如 GPIOA, GPIOE） */
    uint32_t Pin;           /* GPIO 引脚号（如 GPIO_Pin_0） */
    BitAction OnBit;        /* 点亮时写入的电平（Bit_SET=高有效, Bit_RESET=低有效） */
    BitAction OffBit;       /* 熄灭时写入的电平 */
};

#endif /* __LED_DESC_H__ */
