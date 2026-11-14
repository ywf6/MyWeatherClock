/* [阅读说明]
 * 页面状态机：这是按键交互和显示顺序的中心，先读 page_task()，再读数据发布接口。
 * HOME -> FACE(约 600 ms) -> CHAT；K3/主机 HOME 请求返回 HOME。
 * 数据更新者写 home 并置 dirty，页面任务读取快照后提交 UI 消息。
 * revision 标识页面版本，用于拒绝用户离开对话页后才到达的旧回答。
 */
#include <stdbool.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include "page.h"
#include "page_flow.h"
#include "ui.h"

#define KEY_WAKE GPIO_Pin_4
#define KEY_HOME GPIO_Pin_5
#define SCAN_MS 10
#define DEBOUNCE_MS 30
#define WAKE_MS 600

enum {
    HOME_WIFI = 1 << 0,
    HOME_CLOCK = 1 << 1,
    HOME_INDOOR = 1 << 2,
    HOME_WEATHER = 1 << 3
};

typedef enum {
    PAGE_CLOCK,
    PAGE_WAKE_FACE,
    PAGE_CHAT
} active_page_t;

typedef struct {
    rtc_date_time_t date;
    float indoor_temperature;
    float humidity;
    float outdoor_temperature;
    int weather_code;
    char wifi[64];
    unsigned valid;
} home_data_t;

typedef struct {
    bool raw;
    bool stable;
    TickType_t changed;
} key_state_t;

/* [阅读说明]
 * home.valid 表示哪些字段至少收到过有效数据；dirty 表示哪些字段需要刷新。
 * 当前没有时间戳或失效机制，因此 valid 并不表示读数尚未过期。
 */
static home_data_t home;
static unsigned dirty;
static active_page_t active_page;
static bool page_ready;
static unsigned revision;
static unsigned key2_count;
static int pending_request;
static const char *last_source = "BOOT";
static int chat_update;
static unsigned chat_revision;
static uint8_t chat_bitmap[5616];

/* [阅读说明]
 * 仅接受当前 CHAT 页、当前 revision 的思考状态请求；实际绘图由页面任务完成。
 */
int page_flow_chat_busy(unsigned rev)
{
    int ok;
    taskENTER_CRITICAL();
    ok = page_ready && active_page == PAGE_CHAT && revision == rev;
    if (ok) { chat_update = 1; chat_revision = rev; }
    taskEXIT_CRITICAL();
    return ok;
}

/* [阅读说明]
 * 先验证页面版本，再复制完整点阵并标记更新。
 * 临界区保护共享状态；这里的 5616 字节复制也在临界区内，会暂时屏蔽受保护优先级的中断。
 */
int page_flow_chat_bitmap(unsigned rev, const uint8_t *bits)
{
    int ok;
    taskENTER_CRITICAL();
    ok = page_ready && active_page == PAGE_CHAT && revision == rev;
    if (ok) {
        memcpy(chat_bitmap, bits, sizeof(chat_bitmap));
        chat_update = 2;
        chat_revision = rev;
    }
    taskEXIT_CRITICAL();
    return ok;
}

/* [阅读说明]
 * 调用者只提交请求，页面任务在后续扫描中执行；返回成功代表已接收请求，并不代表切页已绘制完成。
 */
int page_flow_request(int wake)
{
    int result;
    taskENTER_CRITICAL();
    result = !page_ready ? 0 :
        (pending_request || (wake && active_page != PAGE_CLOCK)) ? -1 : 1;
    if (result == 1) pending_request = wake ? 1 : 2;
    taskEXIT_CRITICAL();
    return result;
}

void page_flow_get_status(page_flow_status_t *status)
{
    static const char *names[] = {"HOME", "FACE", "CHAT"};
    taskENTER_CRITICAL();
    status->page = page_ready ? names[active_page] : "BOOT";
    status->source = last_source;
    status->revision = revision;
    status->key2_count = key2_count;
    status->valid = home.valid;
    status->indoor10 = (int)(home.indoor_temperature * 10);
    status->humidity10 = (int)(home.humidity * 10);
    status->outdoor10 = (int)(home.outdoor_temperature * 10);
    status->weather_code = home.weather_code;
    status->date = home.date;
    taskEXIT_CRITICAL();
}

/* [阅读说明]
 * 每次发布页面都递增 revision，并清除旧对话更新；它是迟到回答校验的依据。
 */
static void publish_page(active_page_t page, const char *source)
{
    taskENTER_CRITICAL();
    active_page = page;
    page_ready = true;
    last_source = source;
    ++revision;
    chat_update = 0;
    taskEXIT_CRITICAL();
}

void page_flow_set_wifi(const char *label)
{
    taskENTER_CRITICAL();
    strncpy(home.wifi, label, sizeof(home.wifi) - 1);
    home.wifi[sizeof(home.wifi) - 1] = '\0';
    home.valid |= HOME_WIFI;
    dirty |= HOME_WIFI;
    taskEXIT_CRITICAL();
}

void page_flow_set_clock(const rtc_date_time_t *date)
{
    taskENTER_CRITICAL();
    home.date = *date;
    home.valid |= HOME_CLOCK;
    dirty |= HOME_CLOCK;
    taskEXIT_CRITICAL();
}

void page_flow_set_indoor(float temperature, float humidity)
{
    taskENTER_CRITICAL();
    home.indoor_temperature = temperature;
    home.humidity = humidity;
    home.valid |= HOME_INDOOR;
    dirty |= HOME_INDOOR;
    taskEXIT_CRITICAL();
}

void page_flow_set_weather(float temperature, int code)
{
    taskENTER_CRITICAL();
    home.outdoor_temperature = temperature;
    home.weather_code = code;
    home.valid |= HOME_WEATHER;
    dirty |= HOME_WEATHER;
    taskEXIT_CRITICAL();
}

/* [阅读说明]
 * 临界区内复制数据并取走 dirty 标记，离开临界区后提交绘图，避免在保护共享数据时等待 UI 队列。
 */
static void draw_home(bool full)
{
    home_data_t snapshot;
    unsigned updates;

    taskENTER_CRITICAL();
    snapshot = home;
    updates = full ? snapshot.valid : dirty;
    dirty = 0;
    taskEXIT_CRITICAL();

    if (full) main_page_display();
    if (updates & HOME_WIFI) main_page_redraw_wifi_ssid(snapshot.wifi);
    if (updates & HOME_CLOCK) {
        main_page_redraw_time(&snapshot.date);
        main_page_redraw_date(&snapshot.date);
    }
    if (updates & HOME_INDOOR) {
        main_page_redraw_inner_temperature(snapshot.indoor_temperature);
        main_page_redraw_inner_humidity(snapshot.humidity);
    }
    if (updates & HOME_WEATHER) {
        main_page_redraw_outdoor_temperature(snapshot.outdoor_temperature);
        main_page_redraw_outdoor_weather_icon(snapshot.weather_code);
    }
}

static void draw_wake_face(void)
{
    const uint16_t card = mkcolor(24, 48, 66);
    const uint16_t face = mkcolor(255, 208, 92);

    ui_fill_color(54, 97, 185, 221, card);
    ui_fill_color(77, 120, 162, 184, face);
    ui_fill_color(94, 135, 103, 144, card);
    ui_fill_color(136, 135, 145, 144, card);
    ui_fill_color(101, 161, 106, 168, card);
    ui_fill_color(133, 161, 138, 168, card);
    ui_fill_color(107, 168, 132, 173, card);
    ui_write_string(96, 193, "Hello!", 0xFFFF, card, &font16_maple);
}

static void draw_chat(void)
{
    const uint16_t background = mkcolor(12, 22, 32);
    const uint16_t card = mkcolor(24, 48, 66);
    const uint16_t accent = mkcolor(120, 215, 246);

    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, background);
    ui_fill_color(0, 0, UI_WIDTH - 1, 52, card);
    ui_write_string(16, 10, "AI CHAT", 0xFFFF, card, &font16_maple);
    ui_write_string(16, 30, "Text / voice input", accent, card, &font16_maple);
    ui_fill_color(12, 72, 227, 127, card);
    ui_write_string(22, 80, "YOU", accent, card, &font16_maple);
    ui_write_string(22, 102, "Waiting for voice", 0xFFFF, card, &font16_maple);
    ui_fill_color(12, 144, 227, 211, card);
    ui_write_string(22, 152, "ASSISTANT", accent, card, &font16_maple);
    ui_write_string(22, 176, "Ready to listen", 0xFFFF, card, &font16_maple);
    ui_write_string(16, 286, "K3: Back to clock", 0xFFFF, background, &font16_maple);
}

static bool key_down(uint16_t pin)
{
    return GPIO_ReadInputDataBit(GPIOC, pin) == Bit_RESET;
}

/* [阅读说明]
 * 每 10 ms 采样；原始电平稳定至少 30 ms 后才改变消抖状态，仅按下边沿触发一次。
 */
static bool key_pressed(key_state_t *key, uint16_t pin, TickType_t now)
{
    bool down = key_down(pin);
    if (down != key->raw) {
        key->raw = down;
        key->changed = now;
    }
    if (down != key->stable &&
        (TickType_t)(now - key->changed) >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
        key->stable = down;
        return down;
    }
    return false;
}

/* [阅读说明]
 * 唯一决定页面绘图顺序的任务，优先级 6。
 * 先处理返回请求，再处理唤醒和 FACE 超时；CHAT 页更新时仍检查 revision。
 */
static void page_task(void *parameter)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t wake_started = now;
    key_state_t wake = {key_down(KEY_WAKE), false, now};
    key_state_t back = {key_down(KEY_HOME), false, now};
    active_page_t page = PAGE_CLOCK;
    bool back_pressed, wake_pressed, key2_event;
    int request;
    (void)parameter;

    /* A key held at startup must be released before it can trigger. */
    wake.stable = wake.raw;
    back.stable = back.raw;
    draw_home(true);
    publish_page(page, "BOOT");
    for (;;) {
        now = xTaskGetTickCount();
        back_pressed = key_pressed(&back, KEY_HOME, now);
        wake_pressed = key_pressed(&wake, KEY_WAKE, now);

        taskENTER_CRITICAL();
        request = pending_request;
        pending_request = 0;
        taskEXIT_CRITICAL();
        key2_event = wake_pressed && !back_pressed && request != 2 &&
                     (page == PAGE_CLOCK || page == PAGE_CHAT);

        if ((back_pressed || request == 2) && page != PAGE_CLOCK) {
            page = PAGE_CLOCK;
            draw_home(true);
            publish_page(page, back_pressed ? "K3" : "PC");
        } else if ((wake_pressed || request == 1) && page == PAGE_CLOCK &&
                   !back_pressed && request != 2) {
            page = PAGE_WAKE_FACE;
            draw_wake_face();
            wake_started = xTaskGetTickCount();
            publish_page(page, wake_pressed ? "K2" : "PC");
        } else if (page == PAGE_WAKE_FACE &&
                   (TickType_t)(now - wake_started) >= pdMS_TO_TICKS(WAKE_MS)) {
            page = PAGE_CHAT;
            draw_chat();
            publish_page(page, "AUTO");
        } else if (page == PAGE_CLOCK) {
            draw_home(false);
        } else if (page == PAGE_CHAT && chat_update && chat_revision == revision) {
            /* Only this task orders chat drawing and HOME drawing. */
            if (chat_update == 1) {
                ui_fill_color(12, 64, 227, 271, mkcolor(12, 22, 32));
                ui_write_string(22, 90, "Thinking...", 0xFFFF,
                                mkcolor(12, 22, 32), &font16_maple);
            } else {
                ui_draw_chat_bitmap(chat_bitmap);
            }
            chat_update = 0;
        }
        /* [阅读说明]
         * K2 事件计数交给 host 任务转换为 WCKEY2，再由 C3 转给正在监听的电脑端。
         */
        if (key2_event) {
            taskENTER_CRITICAL();
            ++key2_count;
            taskEXIT_CRITICAL();
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_MS));
    }
}

void page_flow_start(void)
{
    GPIO_InitTypeDef gpio;
    BaseType_t created;
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = KEY_WAKE | KEY_HOME;
    gpio.GPIO_Mode = GPIO_Mode_IN;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL; /* V1.2.6 has external pull-ups. */
    GPIO_Init(GPIOC, &gpio);
    created = xTaskCreate(page_task, "pages", 512, NULL, 6, NULL);
    configASSERT(created == pdPASS);
}
