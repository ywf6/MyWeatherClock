/* [阅读说明]
 * 主机控制台头文件：声明 host_console_start() 接口。
 * host_console 模块负责处理 USART1（本地控制台）和 USART3（外置 C3）的 @WC1 命令。
 * 命令语义包括：HELLO 握手、STATUS 状态查询、WAKE/HOME 切页、THINK/LCD* 对话位图等。
 */
#ifndef WEATHER_CLOCK_HOST_CONSOLE_H
#define WEATHER_CLOCK_HOST_CONSOLE_H

/* 初始化 ai_link（USART3）、注册 USART1 接收回调、创建 host 任务（优先级 5，栈深 768 字） */
void host_console_start(void);
#endif
