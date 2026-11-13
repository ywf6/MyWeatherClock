/* [阅读说明]
 * 板级初始化：阅读 main.c 时先理解各初始化函数的职责，再回来看外设时钟。
 * 外设寄存器、GPIO 复用等细节由 driver/ 下各模块负责。
 */
#include <stdint.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include "tim_delay.h"
#include "console.h"
#include "rtc.h"
#include "aht20.h"

/* [阅读说明]
 调度器启动前打开外设时钟，并选择 LSE 作为 RTC 时钟源。
 因为后续代码和调度器启动过程就要用这些外设，且调度器后并发导致时序不可控、易竞争
 外设时钟使能属于板级/驱动层职责，放在 board_lowlevel_init() 里，分层清晰，可移植性好。
 开外设门控时钟（让 GPIO、串口、I2C、SPI、定时器、DMA、PWR 能用）；
解锁备份域（RTC/LSE 在备份域，不解锁写不进去）；
启动 LSE 晶振并等它稳定；
把 RTC 的时钟源选为 LSE。
 */
void board_lowlevel_init(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    RCC_LSEConfig(RCC_LSE_ON);
    /* [阅读说明]
     * 现状提醒：这里没有超时；LSE 未起振时启动流程会停在此处。
     */
    while(RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET);
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
}//这里只开时钟和部分使能

/* [阅读说明]
 * 运行在 init 任务中；传感器初始化使用 vTaskDelay，因此放在调度器启动之后。
 */
void board_init(void)
{
    tim_delay_init();
    console_init();
    printf("[SYS] Build Date: %s %s\n", __DATE__, __TIME__);
    
    rtc_init();
    aht20_init();
}

/* [阅读说明]
 * printf 的字符输出重定向到 USART1，当前路径逐字节等待 TXE。
 * console.c 虽提供 DMA 发送接口，printf 并未调用该接口。
 */
int fputc(int ch, FILE *f)
{
    USART_SendData(USART1, (uint8_t)ch);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    return ch;
}

/* ==========================================================================
 * FreeRTOS 系统级调试钩子（Hook）函数
 *
 * 说明：
 *   以下三个函数是 FreeRTOS 内核声明、由用户实现的回调（覆盖内核弱符号）。
 *   内核在特定事件发生时主动调用它们。是否被调用取决于 FreeRTOSConfig.h：
 *     - vAssertCalled                  ← 由 configASSERT(x) 宏展开调用
 *     - vApplicationStackOverflowHook  ← 需 configCHECK_FOR_STACK_OVERFLOW >= 1
 *     - vApplicationMallocFailedHook   ← 需 configUSE_MALLOC_FAILED_HOOK == 1
 * ========================================================================== */

/**
 * @brief  断言失败处理函数（configASSERT 的落地实现）
 *
 * 触发时机：
 *   任意 configASSERT(x) 断言失败（x == 0）时被调用，
 *   用于捕获空指针、参数越界、内核一致性检查失败等非法状态。
 *
 * @param  file  触发断言的源文件名（__FILE__）
 * @param  line  触发断言的行号（__LINE__）
 *
 * 当前实现：
 *   1. portDISABLE_INTERRUPTS() 关闭中断，冻结现场；
 *   2. printf 打印文件名和行号，便于定位。
 */
void vAssertCalled(const char *file, int line)
{
    portDISABLE_INTERRUPTS();                          /* 关中断，冻结现场 */
    printf("Assert Called: %s(%d)\n", file, line);     /* 打印源文件与行号 */
}

/**
 * @brief  任务栈溢出钩子
 *
 * 触发时机：
 *   任务切换时内核检查任务栈尾填充魔数是否被破坏，若溢出则调用本函数。
 *   前提：configCHECK_FOR_STACK_OVERFLOW >= 1。
 *
 * @param  xTask       发生栈溢出的任务句柄
 * @param  pcTaskName  发生栈溢出的任务名
 *
 * 当前实现：
 *   1. printf 打印溢出任务名；
 *   2. configASSERT(0) 触发断言（进而调用 vAssertCalled）。
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("Stack Overflowed: %s\n", pcTaskName);     /* 打印溢出任务名 */
    configASSERT(0);                                  /* 触发断言 */
}

/**
 * @brief  动态内存（堆）分配失败钩子
 *
 * 触发时机：
 *   pvPortMalloc() 返回 NULL、堆内存耗尽时被内核调用。
 *   前提：configUSE_MALLOC_FAILED_HOOK == 1。
 *
 * 当前实现：
 *   1. printf 打印 "Malloc Failed"；
 *   2. configASSERT(0) 触发断言（进而调用 vAssertCalled）。
 */
void vApplicationMallocFailedHook(void)
{
    printf("Malloc Failed\n");                        /* 打印堆分配失败信息 */
    configASSERT(0);                                  /* 触发断言 */
}
