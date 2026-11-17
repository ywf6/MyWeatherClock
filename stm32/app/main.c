/* [阅读说明]
 * 阅读入口：本文件串起 STM32 启动流程。
 * 先读 main()，再读 main_init()；随后依次查看 app.c、workqueue.c、page/page_flow.c 和 ui.c。
 * board_lowlevel_init() 在调度器启动前运行，main_init() 则是调度器启动后的初始化任务。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "workqueue.h"
#include "app.h"
#include "ui.h"
#include "page.h"
#include "page_flow.h"
#include "host_console.h"

extern void board_lowlevel_init(void);
extern void board_init(void);

/* [阅读说明]
 * 初始化任务，优先级 9。此时调度器已运行，可以使用会阻塞当前任务的 FreeRTOS API。
 */
static void main_init(void *param)
{
    /* [阅读说明]
     * 初始化延时基准、调试串口、RTC 和 AHT20；具体实现在 board.c。
     */
    board_init();
    /* [阅读说明]
     * 创建 host 任务并初始化 USART3；USART1 接收用于本地控制台，USART3 用于外置 C3。
     */
    host_console_start();
    /* [阅读说明]
     * 创建 UI 消息队列和 UI 任务；后续页面函数将绘制请求入队，由 UI 任务统一操作屏幕。
     */
    ui_init();

    welcome_page_display();

    /* 先启动时钟页面和业务，联网由工作队列处理。 */
    page_flow_start();
    app_init();

    /* [阅读说明]
     * 初始化完成后删除当前任务。业务继续由其他任务和软件定时器驱动。
     */
    vTaskDelete(NULL);
}

/* [阅读说明]
 * 复位后的 C 入口；更早的向量表、SystemInit 和 C 运行时初始化在启动文件中。
 */
int main(void)
{
    board_lowlevel_init();
    workqueue_init();

    /* [阅读说明]
     * 栈深 1024 的单位是 StackType_t；当前 Cortex-M4 移植中约为 4 KiB，优先级数值越大越高。
     */
    xTaskCreate(main_init, "init", 1024, NULL, 9, NULL);

    vTaskStartScheduler();

    while (1)
    {
        ; // code should not run here
    }
}
