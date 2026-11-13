/* [阅读说明]
 * 本地调试控制台头文件：声明 USART1 初始化、DMA 发送和接收回调注册接口。
 * USART1: PA9=TX, PA10=RX, 115200 8N1，用于本地 printf 输出和 @WC1 命令接收。
 * 发送支持 DMA2 Stream7 异步传输和轮询两种方式（printf 当前走轮询）。
 */
#ifndef __CONSOLE_H__
#define __CONSOLE_H__

#include <stdint.h>

/* 接收回调函数类型：每收到一个字节调用一次，运行在中断上下文 */
typedef void (*console_received_func_t)(uint8_t data);

/* 初始化 USART1（GPIO/USART/DMA/NVIC），创建发送完成信号量 */
void console_init(void);

/* DMA 异步发送字符串：启动 DMA2 Stream7 并阻塞等待完成信号量。
 * 支持超长文本分块发送，每块最多 65535 字节 */
void console_write(const char str[]);

/* 注册接收回调：收到字节时在中断上下文调用。
 * host_console_start() 注册此回调用于接收本地 @WC1 命令 */
void console_received_register(console_received_func_t func);

#endif /* __CONSOLE_H__ */
