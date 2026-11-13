/* [阅读说明]
 * 联网启动流程：通过 USART2 操作板载 ESP-AT，与外置语音 C3 的联网配置相互独立。
 * 本文件在 init 任务中执行；只有成功返回后，main_init() 才会启动页面和业务定时器。
 */
#include <stdint.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "esp_at.h"
#include "page.h"
#include "wifi.h"

/* [阅读说明]
 * 依次初始化 AT 模块、Wi-Fi 模式和东八区 SNTP；失败转到 err。
 */
void wifi_init(void)
{
    if (!esp_at_init())
    {
        printf("[AT] init failed\n");
        goto err;
    }
    printf("[AT] inited\n");
    
    if (!esp_at_wifi_init())
    {
        printf("[WIFI] init failed\n");
        goto err;
    }
    printf("[WIFI] inited\n");
    
    if (!esp_at_sntp_init())
    {
        printf("[SNTP] init failed\n");
        goto err;
    }
    printf("[SNTP] inited\n");
    
    return;
    
err:
    /* [阅读说明]
     * 现状提醒：后面的无限空转不会主动阻塞高优先级 init 任务，可能使低优先级 UI/通信任务得不到运行。
     */
    error_page_display("wireless init failed");
    while (1)
    {
        ;
    }
}

/* [阅读说明]
 * WIFI_SSID 非空时主动连接，再循环检查状态。
 * 循环中的 AT 查询自身也有超时，因此这里的 10 秒计数不等于严格的墙钟超时。
 */
void wifi_wait_connect(void)
{
    printf("[WIFI] connecting\n");
    
    if (WIFI_SSID[0])
        esp_at_connect_wifi(WIFI_SSID, WIFI_PASSWD, NULL);
    
    for (uint32_t t = 0; t < 10 * 1000; t += 100)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_info_t wifi = { 0 };
        if (esp_at_get_wifi_info(&wifi) && wifi.connected)
        {
            printf("[WIFI] Connected\n");
            printf("[WIFI] SSID: %s, BSSID: %s, Channel: %d, RSSI: %d\n",
                wifi.ssid, wifi.bssid, wifi.channel, wifi.rssi);
            return;
        }
    }
    
    printf("[WIFI] Connection Timeout\n");
    /* [阅读说明]
     * 现状提醒：连接失败后同样进入空转；本轮注释保留原行为，离线运行需另行改造。
     */
    error_page_display("wireless connect failed");
    while (1)
    {
        ;
    }
}
