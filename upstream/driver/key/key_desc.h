/* [阅读说明]
 * 按键描述符结构体定义：描述一个按键的完整硬件配置。
 * 包含 GPIO 端口/引脚、EXTI 线路映射、中断号和回调函数指针。
 */
#ifndef __KEY_DESC_H__
#define __KEY_DESC_H__

#include <stdint.h>
#include "stm32f4xx.h"
#include "key.h"

/* 按键描述符结构体 */
struct key_desc
{
    GPIO_TypeDef *port;         /* GPIO 端口（如 GPIOA, GPIOC） */
    uint16_t pin;               /* GPIO 引脚号（如 GPIO_Pin_0） */
    uint8_t exti_port_src;      /* EXTI 端口源（如 EXTI_PortSourceGPIOA） */
    uint8_t exti_pin_src;       /* EXTI 引脚源（如 EXTI_PinSource0） */
    uint32_t exti_line;         /* EXTI 线（如 EXTI_Line0） */
    uint8_t irqn;               /* NVIC 中断号（如 EXTI0_IRQn） */
    key_func_t func;            /* 按下回调函数指针，NULL 表示未注册 */
};

#endif /* __KEY_DESC_H__ */
