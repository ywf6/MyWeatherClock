/* [阅读说明]
 * 工作队列：生产者投递函数指针和参数，唯一的 workqueue 任务按顺序执行。
 * 这里没有为每项作业单独创建任务，因此 HTTP、校时和传感器作业之间仍然串行。
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "workqueue.h"

typedef struct
{
    work_t work;
    void *param;
} work_message_t;

static QueueHandle_t work_msg_queue;

/* [阅读说明]
 * 无作业时阻塞等待队列，让出 CPU；收到消息后在本任务上下文中调用作业。
 */
static void work_func(void *param)
{
    work_message_t msg;
    
    while (1)
    {
        xQueueReceive(work_msg_queue, &msg, portMAX_DELAY);
        msg.work(msg.param);
    }
}

/* [阅读说明]
 * 队列容量 16，保存 work_message_t 的副本；消费者任务优先级 5，栈深 1024。
 */
void workqueue_init(void)
{
    work_msg_queue = xQueueCreate(16, sizeof(work_message_t));
    configASSERT(work_msg_queue);
    xTaskCreate(work_func, "workqueue", 1024, NULL, 5, NULL);
}

/* [阅读说明]
 * 只复制函数指针和 param 指针，不复制 param 指向的数据；调用方负责其生命周期。
 * 使用 portMAX_DELAY，队列满会阻塞调用者，因此不适合直接从中断调用，也有定时器阻塞风险。
 */
void workqueue_run(work_t work, void *param)
{
    configASSERT(work_msg_queue);
    work_message_t msg = { work, param };
    xQueueSend(work_msg_queue, &msg, portMAX_DELAY);
}
