/* [阅读说明]
 * 按键驱动实现：通用 GPIO+EXTI 按键初始化和读取。
 * 支持 EXTI 上升沿中断检测和电平读取两种方式。
 * 引脚映射：
 *   KEY1: PA0  （唤醒按键，下拉输入，高电平有效）
 *   KEY2: PC4  （唤醒按键，外部上拉，低电平有效）
 *   KEY3: PC5  （返回按键，外部上拉，低电平有效）
 * 注意：当前 page_flow.c 中 K2/K3 采用轮询扫描+消抖方式，
 *       此驱动的 EXTI 中断方式为通用接口，未在主流程中调用。
 */
#include <stdbool.h>
#include <stddef.h>
#include "stm32f4xx.h"
#include "key.h"
#include "key_desc.h"

// KEY1: PA0
// KEY2: PC4
// KEY3: PC5

/* 初始化按键：配置 GPIO 输入模式 + EXTI 上升沿中断 + NVIC
 * 参数 key 为按键描述符，包含端口/引脚/EXTI 线/中断号等信息 */
void key_init(key_desc_t key)
{
    /* GPIO 初始化：下拉输入模式（适配外部低电平有效的按键） */
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Medium_Speed;
    
    /* EXTI 初始化：中断模式，上升沿触发（按下瞬间产生中断） */
    EXTI_InitTypeDef EXTI_InitStructure;
    EXTI_StructInit(&EXTI_InitStructure);
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    
    /* NVIC 初始化：抢占优先级 5，子优先级 0 */
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    
    /* 按描述符配置 GPIO、EXTI 线映射和 NVIC 中断通道 */
    GPIO_InitStructure.GPIO_Pin = key->pin;
    GPIO_Init(key->port, &GPIO_InitStructure);
    SYSCFG_EXTILineConfig(key->exti_port_src, key->exti_pin_src);
    EXTI_InitStructure.EXTI_Line = key->exti_line;
    EXTI_Init(&EXTI_InitStructure);
    NVIC_InitStructure.NVIC_IRQChannel = key->irqn;
    NVIC_Init(&NVIC_InitStructure);
}

/* 读取按键当前电平状态
 * 返回 true 表示引脚为高电平（按下），false 表示低电平（释放） */
bool key_read(key_desc_t key)
{
    return GPIO_ReadInputDataBit(key->port, key->pin) == Bit_SET;
}

/* 注册按键按下回调函数
 * func 将在 EXTI 中断上下文中被调用，应保持短小 */
void key_press_callback_register(key_desc_t key, key_func_t func)
{
    key->func = func;
}
