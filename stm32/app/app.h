/* [阅读说明]
 * 应用层公共头文件：声明业务调度入口 app_init()，并定义应用版本号。
 * app_init() 由 main_init() 在联网成功后调用，创建 5 个软件定时器驱动周期更新。
 */
#ifndef __APP_H__
#define __APP_H__

/* 应用版本号，可通过串口命令 @WC1 HELLO 查询 */
#define APP_VERSION "v1.0"

/* 业务调度初始化：创建时间同步、Wi-Fi 状态、时钟、室内、室外 5 个定时器 */
void app_init(void);

#endif /* __APP_H__ */
