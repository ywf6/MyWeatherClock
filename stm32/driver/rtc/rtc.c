/* [阅读说明]
 * RTC 驱动：把业务层完整年份/日期转换为 STM32 RTC 的字段格式。
 * SNTP 负责周期校准，正常的每秒走时由 RTC 外设维护；LSE 时钟源在 board.c 中选择。
 */
#include <string.h>
#include "stm32f4xx.h"
#include "rtc.h"

void rtc_init(void)
{
    RTC_InitTypeDef RTC_InitStruct;
    RTC_StructInit(&RTC_InitStruct);
    RTC_Init(&RTC_InitStruct);
    
    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForSynchro();
}

/* [阅读说明]
 * RTC 年份使用相对 2000 年的值；RTC_Format_BIN 表示 API 输入为二进制数值。
 */
static void _rtc_set_time_once(const rtc_date_time_t *date_time)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;
    
    RTC_DateStructInit(&date);
    RTC_TimeStructInit(&time);
    
    date.RTC_Year = date_time->year - 2000;
    date.RTC_Month = date_time->month;
    date.RTC_Date = date_time->day;
    date.RTC_WeekDay = date_time->weekday;
    time.RTC_Hours = date_time->hour;
    time.RTC_Minutes = date_time->minute;
    time.RTC_Seconds = date_time->second;
    
    RTC_SetDate(RTC_Format_BIN, &date);
    RTC_SetTime(RTC_Format_BIN, &time);
    
}

static void _rtc_get_time_once(rtc_date_time_t *date_time)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;
    
    RTC_DateStructInit(&date);
    RTC_TimeStructInit(&time);
    
    RTC_GetDate(RTC_Format_BIN, &date);
    RTC_GetTime(RTC_Format_BIN, &time);
    
    date_time->year = 2000 + date.RTC_Year;
    date_time->month = date.RTC_Month;
    date_time->day = date.RTC_Date;
    date_time->weekday = date.RTC_WeekDay;
    date_time->hour = time.RTC_Hours;
    date_time->minute = time.RTC_Minutes;
    date_time->second = time.RTC_Seconds;
}

/* [阅读说明]
 * 写入后读回，直到秒字段相同；当前循环没有次数或超时上限。
 */
void rtc_set_time(const rtc_date_time_t *date_time)
{
    rtc_date_time_t rtime;
    do {
        _rtc_set_time_once(date_time);
        _rtc_get_time_once(&rtime);
    } while (date_time->second != rtime.second);
}

/* [阅读说明]
 * 重复读取并比较整个结构体，两次相同后返回；这是当前代码的一致性检查方式。
 */
void rtc_get_time(rtc_date_time_t *date_time)
{
    rtc_date_time_t time1, time2;
    do {
        _rtc_get_time_once(&time1);
        _rtc_get_time_once(&time2);
    } while (memcmp(&time1, &time2, sizeof(rtc_date_time_t)) != 0);
    
    memcpy(date_time, &time1, sizeof(rtc_date_time_t));
}
