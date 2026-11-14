/* [阅读说明]
 * 页面绘制接口头文件：声明各页面的显示和局部重绘函数。
 * welcome_page / error_page / wifi_page 在初始化阶段调用；
 * main_page 系列函数由 page_flow.c 根据 dirty 标记选择性调用。
 * 局部重绘函数只更新指定区域，避免整屏刷新带来的闪烁。
 */
#ifndef __PAGE_H__
#define __PAGE_H__

#include "rtc.h"

/* 显示欢迎页：黑底 + 梅花 Logo + "天气时钟" 标题 + "loading..." */
void welcome_page_display(void);

/* 显示错误页：黑底 + 错误图标 + 居中错误消息（黄色） */
void error_page_display(const char *msg);

/* 显示 Wi-Fi 连接页：黑底 + Wi-Fi 图标 + SSID + "连接中" 提示 */
void wifi_page_display(void);

/* 绘制主页完整静态布局：时钟区、室内环境区、室外天气区 */
void main_page_display(void);

/* 局部重绘：Wi-Fi SSID 文本 */
void main_page_redraw_wifi_ssid(const char *ssid);

/* 局部重绘：时钟（HH:MM，冒号秒闪） */
void main_page_redraw_time(rtc_date_time_t *time);

/* 局部重绘：日期（YYYY/MM/DD 星期X） */
void main_page_redraw_date(rtc_date_time_t *date);

/* 局部重绘：室内温度（整数，2 位） */
void main_page_redraw_inner_temperature(float temperature);

/* 局部重绘：室内湿度（整数，2 位） */
void main_page_redraw_inner_humidity(float humidity);

/* 局部重绘：室外城市名 */
void main_page_redraw_outdoor_city(const char *city);

/* 局部重绘：室外温度（整数，2 位） */
void main_page_redraw_outdoor_temperature(float temperature);

/* 局部重绘：室外天气图标（按心知天气代码映射到对应图标） */
void main_page_redraw_outdoor_weather_icon(const int code);

#endif /* __PAGE_H__ */
