/* [阅读说明]
 * 按键驱动头文件：声明按键描述符类型和初始化、读取、回调注册接口。
 * 按键采用 EXTI 上升沿中断方式检测按下事件。
 * KEY1: PA0（唤醒，低电平有效，下拉输入）
 * KEY2: PC4（唤醒，外部上拉）
 * KEY3: PC5（返回主页，外部上拉）
 * 注意：当前 page_flow.c 中 K2/K3 采用轮询扫描方式，此驱动为通用接口保留。
 */
#ifndef __KEY_H__
#define __KEY_H__

#include <stdbool.h>
#include <stdint.h>

struct key_desc;
typedef struct key_desc *key_desc_t;    /* 不透明指针类型，指向按键描述符 */

/* 按键按下回调函数类型 */
typedef void (*key_func_t)(key_desc_t key);

/* 初始化按键：配置 GPIO 输入和 EXTI 上升沿中断 */
void key_init(key_desc_t key);

/* 读取按键当前电平状态：返回 true 表示按下（高电平） */
bool key_read(key_desc_t key);

/* 注册按键按下回调函数（在 EXTI 中断中调用） */
void key_press_callback_register(key_desc_t key, key_func_t func);

#endif /* __KEY_H__ */
