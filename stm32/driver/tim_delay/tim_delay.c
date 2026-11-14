/* [阅读说明]
 * TIM6 微秒时间基准模块：提供精确的微秒/毫秒时间戳和忙等延时。
 *
 * 设计原理：
 *   TIM6 挂在 APB1 总线上。STM32F4 时钟树规则：当 APB1 预分频 ≠ 1 时，
 *   挂在 APB1 上的定时器输入时钟自动 ×2。本项目 APB1=42MHz（HCLK/4），
 *   所以 TIM6 输入时钟 = 42×2 = 84 MHz。
 *
 *   预分频设为 84-1，计数频率 = 84MHz/84 = 1 MHz，即每 tick = 1 us。
 *   Period 设为 999，计数器从 0 计到 999 后溢出，每 1000 us = 1 ms 产生一次更新中断。
 *
 *   软件维护 64 位微秒累计值 tim_tick_count，每次中断加 1000；
 *   读时间戳时取「软件累计值 + 硬件当前计数值」，实现 64 位微秒级时间基准。
 *
 * 与 FreeRTOS SysTick 的区别：
 *   - SysTick：FreeRTOS 调度节拍（1ms），由内核管理，任务级等待用 vTaskDelay。
 *   - TIM6：微秒级精度，底层驱动短延时（如 SPI/I2C 时序控制）用 tim_delay_us。
 *   两者独立运行互不干扰。
 *
 * 调用关系：board_init() → tim_delay_init()
 */
#include <stdint.h>
#include <string.h>
#include "stm32f4xx.h"
#include "tim_delay.h"

/* 软件微秒累计值：每次 TIM6 溢出中断（1ms）加 1000。
 * volatile 因为在中断和普通上下文中都被读写 */
static volatile uint64_t tim_tick_count;

/* 周期回调函数指针：每次 TIM6 溢出中断中调用。
 * NULL 表示未注册。非线程安全，应在初始化阶段注册 */
static tim_periodic_callback_t periodic_callback;

/* [阅读说明]
 * 初始化 TIM6 定时器：动态读取 APB1 时钟频率，自动适配预分频值。
 *
 * 时钟链路：HSE(8MHz) → PLL(×168/÷2=168MHz) → AHB(÷1=168MHz)
 *           → APB1(÷4=42MHz) → 定时器×2 = 84MHz
 *
 * 预分频 = 84-1 → 计数频率 1MHz（1us/tick）
 * Period   = 999 → 每 1000 tick 溢出一次（1ms 中断）
 *
 * NVIC 优先级：抢占 5（低于 FreeRTOS 关键中断），子优先级 0。
 * 与 TIM6_DAC_IRQn 共用中断号（STM32F4 中 DAC 和 TIM6 共享同一 IRQ）。
 */
void tim_delay_init(void)
{
    /* 动态读取 PCLK1 频率，乘以 2 得到定时器输入时钟，再换算为 MHz */
    RCC_ClocksTypeDef RCC_ClocksStruct;
    RCC_GetClocksFreq(&RCC_ClocksStruct);
    uint32_t apb1_tim_freq_mhz = RCC_ClocksStruct.PCLK1_Frequency / 1000 / 1000 * 2;
    /* 结果 = 42MHz / 1M * 2 = 84（MHz），即定时器输入时钟 84MHz */

    /* 配置 TIM6 时基：预分频使 1MHz 计数，Period 使 1ms 溢出 */
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_TimeBaseStructInit(&TIM_TimeBaseStructure);
    TIM_TimeBaseStructure.TIM_Prescaler = apb1_tim_freq_mhz - 1;  /* 84-1=83, 84MHz/84=1MHz */
    TIM_TimeBaseStructure.TIM_Period = 999;                         /* 0~999, 1000次=1ms */
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;         /* 不再分频（滤波用） */
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;     /* 向上计数 */
    TIM_TimeBaseInit(TIM6, &TIM_TimeBaseStructure);

    /* 使能更新中断并启动定时器 */
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM6, ENABLE);

    /* 配置 NVIC：TIM6 和 DAC 共用同一中断号 TIM6_DAC_IRQn */
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = TIM6_DAC_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;  /* 抢占优先级5，低于FreeRTOS关键中断 */
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;         /* 子优先级0 */
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

/* [阅读说明]
 * 获取当前微秒时间戳。
 *
 * 实现：软件累计值(tim_tick_count) + 硬件计数器(TIM_GetCounter)。
 * tim_tick_count 是 64 位，但硬件计数器只有 16 位（0~999），两者相加得到完整时间戳。
 *
 * 一致性检查（无锁读）：
 *   读取 tim_tick_count 后再读硬件计数器，期间可能发生中断使 tim_tick_count 变化。
 *   通过 do-while 比较「读取前后的 tim_tick_count」是否变化来检测：
 *   - 未变化：读取期间无中断，结果有效。
 *   - 变化：读取期间发生了中断，重读。
 *   这是一种避免关中断的乐观读策略，比 critical section 开销更小。
 */
uint64_t tim_now(void)
{
    uint64_t now, last_count;
    do {
        last_count = tim_tick_count;                /* 快照软件累计值 */
        now = tim_tick_count + TIM_GetCounter(TIM6);/* 加上硬件当前计数值 */
    } while (last_count != tim_tick_count);         /* 期间若发生中断则重读 */
    return now;
}

/* 获取当前微秒数，直接返回 tim_now() */
uint64_t tim_get_us(void)
{
    return tim_now();
}

/* [阅读说明]
 * 返回时间基准的毫秒数；函数名里的 cpu 不代表 CPU 利用率统计。
 * 注意：头文件声明为 tim_get_ms()，但此处实现函数名为 cpu_get_ms()，
 *       存在命名不一致问题（头文件声明与实现不匹配，链接时可能报错）。
 */
uint64_t cpu_get_ms(void)
{
    return tim_now() / 1000;    /* 微秒转毫秒 */
}

/* [阅读说明]
 * 微秒级忙等延时。
 * 原理：记录起始时间戳，持续查询当前时间戳直到差值 >= us。
 * 特点：精度高（1us），但占用 CPU（不让出调度），不适合长延时。
 * 中断仍可打断此函数执行，不影响延时精度（基于时间戳而非循环计数）。
 */
void tim_delay_us(uint32_t us)
{
    uint64_t now = tim_now();
    while (tim_now() - now < (uint64_t)us);    /* 忙等直到时间差 >= us */
}

/* [阅读说明]
 * 毫秒级忙等延时。
 * 基于微秒计数器实现，乘以 1000 转换。
 * 注意：此函数占用 CPU 不让出调度，任务级等待应优先使用 vTaskDelay()。
 *       仅用于调度器启动前的初始化阶段或极短延时场景。
 */
void tim_delay_ms(uint32_t ms)
{
    uint64_t now = tim_now();
    while (tim_now() - now < (uint64_t)ms * 1000);   /* 忙等直到时间差 >= ms*1000 us */
}

/* 注册周期回调函数：在 TIM6 溢出中断中调用（约每 1ms 一次）。
 * 非线程安全，应在初始化阶段（调度器启动前或单线程上下文）调用。
 * callback 可为 NULL 取消注册 */
void tim_register_periodic_callback(tim_periodic_callback_t callback)
{
    periodic_callback = callback;
}

/* [阅读说明]
 * TIM6 溢出中断服务函数（与 DAC 共用 TIM6_DAC_IRQn）。
 *
 * 执行流程：
 *   1. 检查是否是更新中断（TIM_IT_Update）
 *   2. 清除中断标志（必须，否则反复进入）
 *   3. 软件累计值 += 1000（本次溢出对应的 1000 us）
 *   4. 若注册了周期回调则调用
 *
 * 注意：此函数运行在中断上下文，periodic_callback 应保持短小，
 *       不得调用任何会阻塞的 API（如 vTaskDelay、printf 等）。
 */
void TIM6_DAC_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM6, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM6, TIM_IT_Update);    /* 清除更新中断标志 */
        tim_tick_count += 1000;                         /* 累计 1000 us = 1 ms */
        if (periodic_callback)
            periodic_callback();                        /* 调用注册的周期回调 */
    }
}
