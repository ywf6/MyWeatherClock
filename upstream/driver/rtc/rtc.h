/* [阅读说明]
 * RTC 驱动头文件：声明日期时间结构体和初始化、读写接口。
 * STM32 内置 RTC 外设，使用 LSE（32.768kHz 晶振）作为时钟源。
 * SNTP 负责周期校准，正常走时由 RTC 硬件维护，掉电后由备用电池保持。
 * 年份存储为完整值（如 2025），内部转换为相对 2000 年的偏移。
 */
#ifndef __RTC_H__
#define __RTC_H__

#include <stdint.h>

/* 日期时间结构体：与业务层使用的完整年份格式一致 */
typedef struct
{
    uint16_t year;      /* 完整年份（如 2025） */
    uint8_t month;      /* 月份（1-12） */
    uint8_t day;        /* 日（1-31） */
    uint8_t hour;       /* 时（0-23） */
    uint8_t minute;     /* 分（0-59） */
    uint8_t second;     /* 秒（0-59） */
    uint8_t weekday;    /* 星期（1=周一, 7=周日） */
} rtc_date_time_t;

/* 初始化 RTC 外设，使能 RTC 时钟并等待寄存器同步 */
void rtc_init(void);

/* 设置 RTC 时间。写入后读回验证秒字段一致，不一致则重试 */
void rtc_set_time(const rtc_date_time_t *date_time);

/* 读取 RTC 时间。连续两次读取直到结果一致，避免读取瞬间跨秒导致数据不一致 */
void rtc_get_time(rtc_date_time_t *date_time);

#endif /* __RTC_H__ */
