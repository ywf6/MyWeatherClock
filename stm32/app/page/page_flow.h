/* [阅读说明]
 * 页面流程状态机头文件：声明页面状态查询、请求切换和数据发布接口。
 * 页面流程：HOME（时钟主页） -> WAKE_FACE（约 600ms 过渡） -> CHAT（AI 对话页）
 * K3 按键或主机 HOME 命令可从任意页面返回 HOME。
 * revision 标识页面版本号，用于拒绝用户离开对话页后才到达的过期回答。
 */
#ifndef WEATHER_CLOCK_PAGE_FLOW_H
#define WEATHER_CLOCK_PAGE_FLOW_H

#include "rtc.h"

/* 页面状态快照：供主机串口命令查询当前页面和传感器数据 */
typedef struct {
    const char *page;           /* 当前页面名："HOME" / "FACE" / "CHAT" / "BOOT" */
    const char *source;         /* 切页来源："BOOT" / "K2" / "K3" / "PC" / "AUTO" */
    unsigned revision;          /* 页面版本号，每次切页递增 */
    unsigned key2_count;        /* K2 按键累计触发次数，host 任务上报给 C3 */
    unsigned valid;             /* 已收到有效数据的字段位掩码 */
    int indoor10, humidity10, outdoor10, weather_code; /* 整数*10 表示，避免浮点传输 */
    rtc_date_time_t date;       /* 当前 RTC 时间快照 */
} page_flow_status_t;

/* 提交页面切换请求。wake=1 表示唤醒（HOME->FACE->CHAT），wake=0 表示返回 HOME。
 * 返回 1=已接受，0=页面任务尚未启动，-1=状态不允许或已有待处理请求 */
int page_flow_request(int wake);

/* 获取页面状态快照（线程安全） */
void page_flow_get_status(page_flow_status_t *status);

/* 在 CHAT 页面显示 "Thinking..." 状态。rev 须匹配当前 revision，否则返回 0 */
int page_flow_chat_busy(unsigned rev);

/* 在 CHAT 页面显示 216x208 位图。rev 须匹配当前 revision，否则返回 0 */
int page_flow_chat_bitmap(unsigned rev, const uint8_t *bits);

/* 启动页面状态机任务（优先级 6，栈深 512 字），初始化 K2/K3 按键 GPIO */
void page_flow_start(void);

/* 数据发布接口：更新 Wi-Fi 标签并标记 dirty */
void page_flow_set_wifi(const char *label);

/* 数据发布接口：更新时钟数据并标记 dirty */
void page_flow_set_clock(const rtc_date_time_t *date);

/* 数据发布接口：更新室内温湿度并标记 dirty */
void page_flow_set_indoor(float temperature, float humidity);

/* 数据发布接口：更新室外天气和温度代码并标记 dirty */
void page_flow_set_weather(float temperature, int code);

#endif
