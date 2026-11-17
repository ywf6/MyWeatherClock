/* [阅读说明]
 * Wi-Fi 配置头文件：定义 SSID、密码和心知天气 API Key，声明联网启动接口。
 * 这些宏在编译时写死，留空表示使用 ESP-AT 模块的默认连接或跳过对应功能。
 * WIFI_SSID 非空时，后台联网会优先使用指定热点。
 */
#ifndef __WIFI_H__
#define __WIFI_H__

#include "esp_at.h"

/* Wi-Fi 热点名称，留空时使用板载 ESP-AT 已保存的热点 */
#define WIFI_SSID   ""

/* Wi-Fi 密码，与 WIFI_SSID 配套使用 */
#define WIFI_PASSWD ""

/* 心知天气 API Key，留空则跳过室外天气获取 */
#define WC_SENIVERSE_KEY ""

/* 在工作队列中查询连接状态，必要时尝试联网。 */
bool wifi_poll(esp_wifi_info_t *info);

#endif /* __WIFI_H__ */
