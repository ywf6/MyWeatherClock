/* [阅读说明]
 * 业务调度层：管理时钟、联网状态、室内采集和室外天气的周期更新。
 * 较慢作业由软件定时器投递给唯一的 workqueue 任务，RTC 快速读取直接在定时器回调中执行。
 * 各更新函数只写页面数据，由页面任务决定何时绘制。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "workqueue.h"
#include "rtc.h"
#include "aht20.h"
#include "esp_at.h"
#include "weather.h"
#include "page_flow.h"
#include "app.h"
#include "wifi.h"

#define MILLISECONDS(x) (x)
#define SECONDS(x)      MILLISECONDS((x) * 1000)
#define MINUTES(x)      SECONDS((x) * 60)
#define HOURS(x)        MINUTES((x) * 60)
#define DAYS(x)          HOURS((x) * 24)

#define TIME_SYNC_INTERVAL          HOURS(1)
#define WIFI_UPDATE_INTERVAL        SECONDS(5)
#define TIME_UPDATE_INTERVAL        SECONDS(1)
#define INNER_UPDATE_INTERVAL       SECONDS(3)
#define OUTDOOR_UPDATE_INTERVAL     MINUTES(1)

/* [阅读说明]
 * 这组事件位宏在当前文件中未被使用；实际调度路径是下方的软件定时器和工作队列。
 */
#define MLOOP_EVT_TIME_SYNC         (1 << 0)
#define MLOOP_EVT_WIFI_UPDATE       (1 << 1)
#define MLOOP_EVT_INNER_UPDATE      (1 << 2)
#define MLOOP_EVT_OUTDOOR_UPDATE    (1 << 3)
#define MLOOP_EVT_ALL               (MLOOP_EVT_TIME_SYNC | \
                                     MLOOP_EVT_WIFI_UPDATE | \
                                     MLOOP_EVT_INNER_UPDATE | \
                                     MLOOP_EVT_OUTDOOR_UPDATE)

static TimerHandle_t time_sync_timer;
static TimerHandle_t wifi_update_timer;
static TimerHandle_t time_update_timer;
static TimerHandle_t inner_update_timer;
static TimerHandle_t outdoor_update_timer;
static bool network_connected;

/* [阅读说明]
 * 在 workqueue 任务中经 ESP-AT 获取 SNTP 时间并写 RTC。
 * 使用单次定时器：正常后约 1 小时重试，失败后约 1 秒重试；ChangePeriod 会重新启动定时器。
 */
static void time_sync(void)
{
    uint32_t restart_sync_delay = TIME_SYNC_INTERVAL;
    rtc_date_time_t rtc_date = { 0 };

    esp_date_time_t esp_date = { 0 };
    if (!network_connected || !esp_at_sntp_get_time(&esp_date))
    {
        printf("[SNTP] get time failed\n");
        restart_sync_delay = SECONDS(1);
        goto err;
    }
    
    if (esp_date.year < 2000)
    {
        printf("[SNTP] invalid date formate\n");
        restart_sync_delay = SECONDS(1);
        goto err;
    }
    
    printf("[SNTP] sync time: %04u-%02u-%02u %02u:%02u:%02u (%d)\n",
        esp_date.year, esp_date.month, esp_date.day,
        esp_date.hour, esp_date.minute, esp_date.second, esp_date.weekday);
    
    rtc_date.year = esp_date.year;
    rtc_date.month = esp_date.month;
    rtc_date.day = esp_date.day;
    rtc_date.hour = esp_date.hour;
    rtc_date.minute = esp_date.minute;
    rtc_date.second = esp_date.second;
    rtc_date.weekday = esp_date.weekday;
    rtc_set_time(&rtc_date);
    
err:
    xTimerChangePeriod(time_sync_timer, pdMS_TO_TICKS(restart_sync_delay), 0);
}

/* 联网尝试结束后再启动下一次轮询，避免慢速 AT 命令积压。 */
static void wifi_update(void)
{
    static char last_ssid[64];
    esp_wifi_info_t info = { 0 };
    bool connected = wifi_poll(&info) && info.connected;

    if (connected != network_connected || (connected && strcmp(last_ssid, info.ssid) != 0)) {
        page_flow_set_wifi(connected ? info.ssid : "Wi-Fi offline");
        strcpy(last_ssid, connected ? info.ssid : "");
    }
    network_connected = connected;
    xTimerChangePeriod(wifi_update_timer, pdMS_TO_TICKS(WIFI_UPDATE_INTERVAL), 0);
}

/* [阅读说明]
 * 直接运行在软件定时器服务任务中；只读 RTC 和更新页面数据，不执行网络请求或屏幕传输。
 */
static void time_update(void)
{
    static rtc_date_time_t last_date = { 0 };
    
    rtc_date_time_t date;
    rtc_get_time(&date);
    
    if (date.year < 2020)
    {
        return;
    }
    
    if (memcmp(&date, &last_date, sizeof(rtc_date_time_t)) == 0)
    {
        return;
    }
    
    memcpy(&last_date, &date, sizeof(rtc_date_time_t));
    page_flow_set_clock(&date);
}

/* [阅读说明]
 * 在 workqueue 中执行“触发测量 -> 等待 -> 读取”，测量等待期间可让出 CPU。
 * 与其他慢作业共享同一个消费者任务，遇到耗时网络操作时可能延后。
 */
static void inner_update(void)
{
    static float last_temperature, last_humidity;
    
    if (!aht20_start_measurement())
    {
        printf("[AHT20] start measurement failed\n");
        return;
    }
    
    if (!aht20_wait_for_measurement())
    {
        printf("[AHT20] wait for measurement failed\n");
        return;
    }
    
    float temperature = 0.0f, humidity = 0.0f;
    
    if (!aht20_read_measurement(&temperature, &humidity))
    {
        printf("[AHT20] read measurement failed\n");
        return;
    }
    
    if (temperature == last_temperature && humidity == last_humidity)
    {
        return;
    }
    
    last_temperature = temperature;
    last_humidity = humidity;
    
    printf("[AHT20] Temperature: %.1f, Humidity: %.1f\n", temperature, humidity);
    page_flow_set_indoor(temperature, humidity);
}

/* [阅读说明]
 * 获取西安心知天气实况；未配置 WC_SENIVERSE_KEY 时跳过。
 * 失败时保留旧数据，当前未设置数据过期时间；有效位不等于读数始终新鲜。
 */
static void outdoor_update(void)
{
    static weather_info_t last_weather = { 0 };
    
    weather_info_t weather = { 0 };
    const char *weather_url = "https://api.seniverse.com/v3/weather/now.json?key=" WC_SENIVERSE_KEY "&location=xian&language=zh-Hans&unit=c";
    const char *weather_http_response;
    if (!network_connected || !WC_SENIVERSE_KEY[0])
        return;
    weather_http_response = esp_at_http_get(weather_url);
    if (weather_http_response == NULL)
    {
        printf("[WEATHER] http error\n");
        return;
    }
    
    if (!parse_seniverse_response(weather_http_response, &weather))
    {
        printf("[WEATHER] parse failed\n");
        return;
    }
    
    if (memcmp(&last_weather, &weather, sizeof(weather_info_t)) == 0)
    {
        return;
    }
    
    memcpy(&last_weather, &weather, sizeof(weather_info_t));
    printf("[WEATHER] %s, %s, %.1f\n", weather.city, weather.weather, weather.temperature);
    
    page_flow_set_weather(weather.temperature, weather.weather_code);
}

/* [阅读说明]
 * 用函数指针表示作业；该嵌入式工具链下将作业指针作为 timer ID / 工作参数传递。
 */
typedef void (*app_job_t)(void);

static void app_work(void *param)
{
    app_job_t job = (app_job_t)param;
    job();
}

/* [阅读说明]
 * 运行在软件定时器服务任务中；将耗时作业转交 workqueue。
 * 现状提醒：workqueue_run() 使用无限等待入队，队列满时仍会阻塞定时器服务任务。
 */
static void work_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);
    workqueue_run(app_work, job);
}

/* [阅读说明]
 * 直接执行轻量作业。此回调中的作业应保持短小，并避免进入阻塞状态。
 */
static void app_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);
    job();
}

/* [阅读说明]
 * 创建 5 个软件定时器，它们共享同一个定时器服务任务。
 * 时间 1 秒、联网 5 秒、室内 3 秒、室外 1 分钟；启动时还主动投递一次数据更新。
 */
void app_init(void)
{
    time_update_timer = xTimerCreate("time update", pdMS_TO_TICKS(TIME_UPDATE_INTERVAL), pdTRUE, time_update, app_timer_cb);
    time_sync_timer = xTimerCreate("time sync", pdMS_TO_TICKS(200), pdFALSE, time_sync, work_timer_cb);
    wifi_update_timer = xTimerCreate("wifi update", pdMS_TO_TICKS(WIFI_UPDATE_INTERVAL), pdFALSE, wifi_update, work_timer_cb);
    inner_update_timer = xTimerCreate("inner upadte", pdMS_TO_TICKS(INNER_UPDATE_INTERVAL), pdTRUE, inner_update, work_timer_cb);
    outdoor_update_timer = xTimerCreate("outdoor update", pdMS_TO_TICKS(OUTDOOR_UPDATE_INTERVAL), pdTRUE, outdoor_update, work_timer_cb);

    workqueue_run(app_work, wifi_update);
    workqueue_run(app_work, time_sync);
    workqueue_run(app_work, inner_update);
    workqueue_run(app_work, outdoor_update);
    
    xTimerStart(time_update_timer, 0);
    xTimerStart(inner_update_timer, 0);
    xTimerStart(outdoor_update_timer, 0);
}
