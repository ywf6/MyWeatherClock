/* [阅读说明]
 * 工作队列头文件：声明作业类型和初始化、投递接口。
 * 工作队列模式：生产者（定时器回调）投递函数指针+参数，唯一的 workqueue 任务串行执行。
 * 避免为每个周期作业创建独立任务，减少内存开销和上下文切换。
 */
#ifndef __APP_WORKQUEUE_H__
#define __APP_WORKQUEUE_H__

/* 作业函数类型：接受一个 void* 参数 */
typedef void (*work_t)(void *param);

/* 初始化工作队列：创建消息队列（容量 16）和消费者任务（优先级 5，栈深 1024 字） */
void workqueue_init(void);

/* 投递作业到工作队列。使用 portMAX_DELAY 等待入队，队列满时阻塞调用者。
 * 注意：不适合从中断直接调用；param 指向的数据生命周期由调用方管理 */
void workqueue_run(work_t work, void *param);

#endif /* __APP_WORKQUEUE_H__ */
