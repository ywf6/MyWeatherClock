/* [阅读说明]
 * 微秒延时头文件：基于 TIM6 定时器提供精确时间基准。
 * 与 FreeRTOS SysTick 区分：本模块用于底层短延时（微秒级）和时间查询。
 * 毫秒级任务等待应优先使用 vTaskDelay，微秒级忙等使用 tim_delay_us。
 */
#ifndef __TIM_DELAY_H__
#define __TIM_DELAY_H__

#include <stdint.h>

/* 定时回调函数类型：在 TIM6 溢出中断中调用 */
typedef void (*tim_periodic_callback_t)(void);

/* 初始化 TIM6：根据 APB1 时钟设置 1us 计数频率，约 1ms 溢出中断 */
void tim_delay_init(void);

/* 获取当前微秒时间戳（软件累计 + 硬件计数器，带一致性检查） */
uint64_t tim_now(void);

/* 获取当前微秒数（同 tim_now） */
uint64_t tim_get_us(void);

/* 获取当前毫秒数。
 * 注意：头文件声明为 tim_get_ms()，但 .c 中实现函数名为 cpu_get_ms()，
 *       存在命名不一致（现状提醒），使用时需注意链接问题 */
uint64_t tim_get_ms(void);

/* 微秒级忙等延时：持续查询计数器，占用 CPU */
void tim_delay_us(uint32_t us);

/* 毫秒级忙等延时：基于微秒计数器，占用 CPU。任务级等待优先用 vTaskDelay */
void tim_delay_ms(uint32_t ms);

/* 注册周期回调函数（在 TIM6 溢出中断中调用，约每 1ms 一次） */
void tim_register_periodic_callback(tim_periodic_callback_t callback);

#endif /* __TIM_DELAY_H__ */
