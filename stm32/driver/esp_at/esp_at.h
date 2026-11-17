/* [阅读说明]
 * 板载 ESP-AT 驱动头文件：声明 AT 命令接口和数据结构。
 * ESP-AT 模块通过 USART2（PA2=TX, PA3=RX, 115200）与 STM32 通信。
 * 发送使用 DMA1 Stream6，接收使用 RXNE 中断按行识别 AT 响应。
 * 支持 Wi-Fi 连接管理、SNTP 时间同步和 HTTP GET 请求。
 */
#ifndef __ESP_AT_H__
#define __ESP_AT_H__

#include <stdbool.h>
#include <stdint.h>

/* Wi-Fi 连接信息结构体 */
typedef struct
{
	char ssid[64];       /* SSID 名称 */
	char bssid[18];      /* BSSID（MAC 地址） */
	int channel;         /* 信道号 */
	int rssi;            /* 信号强度（dBm） */
	bool connected;      /* 是否已连接（CWSTATE==2） */
} esp_wifi_info_t;

/* 日期时间结构体（SNTP 返回） */
typedef struct
{
    uint16_t year;      /* 年份（完整 4 位，如 2025） */
    uint8_t month;      /* 月份（1-12） */
    uint8_t day;        /* 日（1-31） */
    uint8_t hour;       /* 时（0-23） */
    uint8_t minute;     /* 分（0-59） */
    uint8_t second;     /* 秒（0-59） */
    uint8_t weekday;    /* 星期（1=周一, 7=周日） */
} esp_date_time_t;

/* 初始化串口并探测 AT 模块，保留模块已保存的设置。 */
bool esp_at_init(void);

/* 设置 Wi-Fi 模式为 Station（AT+CWMODE=1） */
bool esp_at_wifi_init(void);

/* 连接指定热点；ssid 和 pwd 均为 NULL 时使用模块已保存的热点。 */
bool esp_at_connect_wifi(const char *ssid, const char *pwd, const char *mac);

/* 查询 Wi-Fi 连接状态和详情（AT+CWSTATE? + AT+CWJAP?） */
bool esp_at_get_wifi_info(esp_wifi_info_t *info);

/* 简化查询：返回是否已连接 */
bool wifi_is_connected(void);

/* 配置 SNTP 时区为 UTC+8（AT+CIPSNTPCFG=1,8） */
bool esp_at_sntp_init(void);

/* 获取 SNTP 时间（AT+CIPSNTPTIME?），解析英文格式时间文本 */
bool esp_at_sntp_get_time(esp_date_time_t *date);

/* HTTP GET 请求（AT+HTTPCLIENT）。返回内部缓冲指针，下次 AT 命令会覆盖 */
const char *esp_at_http_get(const char *url);

#endif /* __ESP_AT_H__ */
