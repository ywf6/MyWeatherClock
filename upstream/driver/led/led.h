/* [阅读说明]
 * LED 驱动头文件：声明 LED 描述符类型和初始化、开关接口。
 * LED 采用 GPIO 推挽输出控制，支持高有效和低有效两种接法。
 */
#ifndef __LED_H__
#define __LED_H__

#include <stdbool.h>
#include <stdint.h>

struct led_desc;
typedef struct led_desc *led_desc_t;   /* 不透明指针类型，指向 LED 描述符 */

/* 初始化 LED：配置 GPIO 推挽输出并默认关闭 */
void led_init(led_desc_t led);

/* 设置 LED 开关状态：true=开, false=关 */
void led_set(led_desc_t led, bool onoff);

/* 点亮 LED */
void led_on(led_desc_t led);

/* 熄灭 LED */
void led_off(led_desc_t led);

#endif /* __LED_H__ */
