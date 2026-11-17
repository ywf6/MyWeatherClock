/* 通过 USART2 管理板载 ESP-AT。只在 workqueue 中调用，与其他 AT 请求串行。 */
#include <stdio.h>
#include "wifi.h"

bool wifi_poll(esp_wifi_info_t *info)
{
    static bool initialized;
    static bool configured_network_applied;
    static bool sntp_configured;

    if (!initialized) {
        if (!esp_at_init() || !esp_at_wifi_init()) {
            printf("[WIFI] init failed\n");
            return false;
        }
        initialized = true;
    }

    if (!esp_at_get_wifi_info(info))
        return false;

    if (!info->connected || (WIFI_SSID[0] && !configured_network_applied)) {
        bool joined = WIFI_SSID[0]
            ? esp_at_connect_wifi(WIFI_SSID, WIFI_PASSWD, NULL)
            : esp_at_connect_wifi(NULL, NULL, NULL);
        if (!joined)
            return false;
        configured_network_applied = true;
        if (!esp_at_get_wifi_info(info))
            return false;
    }

    if (info->connected && !sntp_configured)
        sntp_configured = esp_at_sntp_init();

    if (!info->connected)
        sntp_configured = false;

    return true;
}
