/* [阅读说明]
 * LED 驱动实现：通用 GPIO 推挽输出 LED 控制器。
 * 支持高有效（OnBit=Bit_SET）和低有效（OnBit=Bit_RESET）两种接法。
 * 通过描述符配置端口/引脚/开关电平，实现多 LED 统一管理。
 * 当前项目未直接调用此驱动，保留供后续扩展。
 */
#include <stdbool.h>
#include <stdint.h>
#include "led_desc.h"
#include "led.h"

/* 初始化 LED：配置 GPIO 推挽输出，100MHz 速率，默认关闭 */
void led_init(led_desc_t led)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = led->Pin;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;          /* 推挽输出 */
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(led->Port, &GPIO_InitStructure);
    
    /* 初始化为关闭状态 */
    GPIO_WriteBit(led->Port, led->Pin, led->OffBit);
}

/* 设置 LED 开关状态 */
void led_set(led_desc_t led, bool onoff)
{
    GPIO_WriteBit(led->Port, led->Pin, onoff ? led->OnBit : led->OffBit);
}

/* 点亮 LED */
void led_on(led_desc_t led)
{
    GPIO_WriteBit(led->Port, led->Pin, led->OnBit);
}

/* 熄灭 LED */
void led_off(led_desc_t led)
{
    GPIO_WriteBit(led->Port, led->Pin, led->OffBit);
}
