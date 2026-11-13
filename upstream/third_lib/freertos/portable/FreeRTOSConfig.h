/* [阅读说明]
 * 本项目 FreeRTOS 配置，阅读业务任务后回来看这里；无需先通读内核源码。
 * 本文件是配置入口，本轮注释未修改第三方内核的调度、队列或内存算法。
 */
#ifndef __FREERTOS_CONFIG_H__
#define __FREERTOS_CONFIG_H__

/* Here is a good place to include header files that are required across
   your application. */
#include "stm32f4xx.h"
extern uint32_t SystemCoreClock;

/* [阅读说明]
 * 启用抢占和时间片；优先级范围由 configMAX_PRIORITIES=10 决定，即 0 到 9。
 */
#define configUSE_PREEMPTION                                        1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION                     1
#define configUSE_TICKLESS_IDLE                                     0
#define configCPU_CLOCK_HZ                                          SystemCoreClock
/* [阅读说明]
 * 1000 Hz 对应 1 ms 的 tick；应用使用 pdMS_TO_TICKS 做时间单位转换。
 */
#define configTICK_RATE_HZ                                          1000
#define configMAX_PRIORITIES                                        10
#define configMINIMAL_STACK_SIZE                                    128
#define configMAX_TASK_NAME_LEN                                     16
#define configUSE_16_BIT_TICKS                                      0
#define configIDLE_SHOULD_YIELD                                     1
#define configUSE_TASK_NOTIFICATIONS                                1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES                       3
#define configUSE_MUTEXES                                           1
#define configUSE_RECURSIVE_MUTEXES                                 1
#define configUSE_COUNTING_SEMAPHORES                               1
#define configUSE_ALTERNATIVE_API                                   0 /* Deprecated! */
#define configQUEUE_REGISTRY_SIZE                                   10
#define configUSE_QUEUE_SETS                                        1
#define configUSE_TIME_SLICING                                      1
#define configUSE_NEWLIB_REENTRANT                                  0
#define configENABLE_BACKWARD_COMPATIBILITY                         0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS                     5
#define configUSE_MINI_LIST_ITEM                                    1
#define configSTACK_DEPTH_TYPE                                      uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE                            size_t

/* Memory allocation related definitions. */
#define configSUPPORT_STATIC_ALLOCATION                             0
#define configSUPPORT_DYNAMIC_ALLOCATION                            1
/* [阅读说明]
 * heap_4 管理的 FreeRTOS 堆为 92 KiB，任务栈、队列和 UI 动态副本从这里分配。
 */
#define configTOTAL_HEAP_SIZE                                       (92 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP                            0

/* Hook function related definitions. */
#define configUSE_IDLE_HOOK                                 0
#define configUSE_TICK_HOOK                                 0
#define configCHECK_FOR_STACK_OVERFLOW                      1
#define configUSE_MALLOC_FAILED_HOOK                        1
#define configUSE_DAEMON_TASK_STARTUP_HOOK                  0
#define configUSE_SB_COMPLETED_CALLBACK                     0

/* Run time and task stats gathering related definitions. */
/* [阅读说明]
 * 当前关闭运行时间统计，因此不能从该配置得出实测 CPU 占用；简历中的数字需区分估算与实测。
 */
#define configGENERATE_RUN_TIME_STATS                       0
#define configUSE_TRACE_FACILITY                            0
#define configUSE_STATS_FORMATTING_FUNCTIONS                0

/* Co-routine related definitions. */
#define configUSE_CO_ROUTINES                               0
#define configMAX_CO_ROUTINE_PRIORITIES                     1

/* Software timer related definitions. */
#define configUSE_TIMERS                                    1
/* [阅读说明]
 * 所有软件定时器回调共用优先级 9 的服务任务；队列 32 是定时器命令队列，不是 workqueue 的作业队列。
 */
#define configTIMER_TASK_PRIORITY                           (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                            32
#define configTIMER_TASK_STACK_DEPTH                        configMINIMAL_STACK_SIZE

/* Interrupt nesting behaviour configuration. */
#define configPRIO_BITS                         4
#define configKERNEL_INTERRUPT_PRIORITY         (15 << (8 - configPRIO_BITS))
/* [阅读说明]
 * 当前允许调用 FreeRTOS FromISR API 的中断要满足该优先级阈值。
 * Cortex-M 的 NVIC 数值越小紧迫性越高，与 FreeRTOS 任务优先级的大小关系相反。
 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (5 << (8 - configPRIO_BITS))
#define configMAX_API_CALL_INTERRUPT_PRIORITY   configMAX_SYSCALL_INTERRUPT_PRIORITY

/* Define to trap errors during development. */
void vAssertCalled(const char *file, int line);
#define configASSERT( x ) if( ( x ) == 0 ) vAssertCalled( __FILE__, __LINE__ )

/* Optional functions - most linkers will remove unused functions anyway. */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_uxTaskGetStackHighWaterMark2    1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1

/* A header file that defines trace macro can be included here. */

#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
#define vPortSVCHandler SVC_Handler

#endif /* FREERTOS_CONFIG_H */

