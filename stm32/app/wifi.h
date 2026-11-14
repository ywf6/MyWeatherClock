/* [阅读说明]
 * Wi-Fi 配置头文件：定义 SSID、密码和心知天气 API Key，声明联网启动接口。
 * 这些宏在编译时写死，留空表示使用 ESP-AT 模块的默认连接或跳过对应功能。
 * WIFI_SSID 非空时，wifi_wait_connect() 会主动连接指定热点。
 */
#ifndef __WIFI_H__
#define __WIFI_H__

#define APP_VERSION "v1.0"

/* Wi-Fi 热点名称，留空则不主动连接，等待 ESP-AT 模块已保存的连接 */
#define WIFI_SSID   ""

/* Wi-Fi 密码，与 WIFI_SSID 配套使用 */
#define WIFI_PASSWD ""

/* 心知天气 API Key，留空则跳过室外天气获取 */
#define WC_SENIVERSE_KEY ""

/* 初始化 ESP-AT 模块、Wi-Fi 模式和 SNTP；失败时显示错误页并阻塞 */
void wifi_init(void);

/* 等待 Wi-Fi 连接成功，最多等待 10 秒；超时后显示错误页并阻塞 */
void wifi_wait_connect(void);

#endif /* __WIFI_H__ */
