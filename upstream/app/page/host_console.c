/* [阅读说明]
 * STM32 命令处理层：USART1 本地控制台和 USART3 外置 C3 共用 @WC1 命令语义。
 * 中断只接收字节；host 任务解析命令、查询状态、提交页面请求和验证点阵。
 * 点阵协议：LCDSTART -> 多个 LCDDATA -> LCDEND，检查会话、页面版本、连续偏移和字节和。
 */
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "console.h"
#include "ai_link.h"
#include "page_flow.h"
#include "host_console.h"
#ifdef WC_OTA_APP
#include "../../../ota/layout.h"
#include "stm32f4xx.h"
#endif

/* ISR only stores bytes. Parsing, printing and page requests run in a task. */
static volatile unsigned head, tail, overflow;
static unsigned char rx[256];
static uint8_t bitmap[5616];
static unsigned bitmap_id, bitmap_rev, bitmap_used, bitmap_sum;
static unsigned bitmap_active;
static bool peer_response, bitmap_peer;
static TickType_t bitmap_activity;

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* [阅读说明]
 * USART1 接收回调，运行在中断上下文；仅写环形缓冲，不在这里解析命令或画屏。
 */
static void receive_byte(uint8_t data)
{
    unsigned next = (head + 1) & 255;
    if (next == tail) { overflow = 1; return; }
    rx[head] = data;
    head = next;
}

/* [阅读说明]
 * peer_response 区分回复去向：C3 命令经 USART3 回复，本地控制台经 USART1 输出。
 * 本地回复暂停调度以减少其他任务 printf 插入同一帧，但未关闭中断。
 */
static void emit(const char *line)
{
    if (peer_response) {
        ai_link_write("@WC1 ");
        ai_link_write(line);
        ai_link_write("\r\n");
        return;
    }
    /* Keep each frame intact against other tasks' printf. IRQs stay enabled. */
    vTaskSuspendAll();
    printf("\r\n@WC1 %s\r\n", line);
    xTaskResumeAll();
}

/* [阅读说明]
 * 发布一份页面和传感器快照；温湿度使用乘 10 的整数表示，避免在线协议中解析浮点。
 */
static void report(void)
{
    page_flow_status_t s;
    char line[256];
    page_flow_get_status(&s);
    snprintf(line, sizeof(line),
        "STATE page=%s source=%s rev=%u uptime_ms=%lu valid=%u "
        "temp10=%d humidity10=%d outdoor10=%d weather=%d "
        "date=%04u-%02u-%02uT%02u:%02u:%02u",
        s.page, s.source, s.revision,
        (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS), s.valid,
        s.indoor10, s.humidity10, s.outdoor10, s.weather_code,
        s.date.year, s.date.month, s.date.day,
        s.date.hour, s.date.minute, s.date.second);
    emit(line);
}

/* [阅读说明]
 * 命令分发入口。LCDSTART 绑定页面 revision，LCDDATA 必须按连续偏移提交，LCDEND 才发布整张图片。
 * ASK/ASKFIX/DRAWFIX 保留历史诊断路径；当前 Qt 主链路由电脑调用模型并发送 LCD 命令。
 */
static void command(const char *line)
{
    int result;
    unsigned id, rev, offset, sum;
    char hex[193], extra;
    char reply[100];
    page_flow_status_t status;
    if (!strcmp(line, "@WC1 HELLO")) {
        emit("HELLO fw=page-flow-voice-v2 board=STM32F407 protocol=1 caps=chat_bitmap,voice_uart,voice_key2"
#ifdef WC_OTA_APP
             ",ota_app"
#endif
        );
        report();
    } else if (sscanf(line, "@WC1 THINK %u %c", &rev, &extra) == 1) {
        emit(page_flow_chat_busy(rev) ? "ACK cmd=THINK" : "ERR reason=STALE_CHAT");
    } else if (sscanf(line, "@WC1 LCDSTART %u %u %u %c", &id, &rev, &sum, &extra) == 3) {
        if (bitmap_active && bitmap_peer != peer_response) {
            emit("ERR reason=DISPLAY_BUSY"); return;
        }
        page_flow_get_status(&status);
        if (strcmp(status.page, "CHAT") || status.revision != rev) {
            emit("ERR reason=STALE_CHAT"); return;
        }
        bitmap_id = id; bitmap_rev = rev; bitmap_sum = sum;
        bitmap_used = 0; bitmap_active = 1;
        bitmap_peer = peer_response;
        bitmap_activity = xTaskGetTickCount();
        snprintf(reply, sizeof(reply), "ACK cmd=LCDSTART id=%u", id);
        emit(reply);
    } else if (sscanf(line, "@WC1 LCDDATA %u %u %192s %c", &id, &offset, hex, &extra) == 3) {
        unsigned i, size = strlen(hex);
        if (!bitmap_active || bitmap_peer != peer_response || id != bitmap_id || offset != bitmap_used ||
            !size || (size & 1) || bitmap_used + size / 2 > sizeof(bitmap)) {
            emit("ERR reason=BAD_CHUNK"); return;
        }
        for (i = 0; i < size; ++i) {
            if (hex_digit(hex[i]) < 0) { emit("ERR reason=BAD_HEX"); return; }
        }
        for (i = 0; i < size; i += 2)
            bitmap[bitmap_used++] = (hex_digit(hex[i]) << 4) | hex_digit(hex[i + 1]);
        bitmap_activity = xTaskGetTickCount();
        snprintf(reply, sizeof(reply), "ACK cmd=LCDDATA id=%u next=%u", id, bitmap_used);
        emit(reply);
    } else if (sscanf(line, "@WC1 LCDEND %u %c", &id, &extra) == 1) {
        unsigned i, actual = 0;
        if (!bitmap_active || bitmap_peer != peer_response || id != bitmap_id || bitmap_used != sizeof(bitmap)) {
            emit("ERR reason=INCOMPLETE_IMAGE"); return;
        }
        bitmap_active = 0;
        for (i = 0; i < sizeof(bitmap); ++i) actual += bitmap[i];
        if (actual != bitmap_sum) { emit("ERR reason=CHECKSUM"); return; }
        if (!page_flow_chat_bitmap(bitmap_rev, bitmap)) { emit("ERR reason=STALE_CHAT"); return; }
        snprintf(reply, sizeof(reply), "ACK cmd=LCDEND id=%u accepted=1", id);
        emit(reply);
    } else if (!strcmp(line, "@WC1 STATUS")) {
        report();
    } else if (!strcmp(line, "@WC1 PING")) {
        emit(ai_link_ping(2000) ? "ACK cmd=PING peer=up" : "ERR reason=PEER_DOWN");
    } else if (!strncmp(line, "@WC1 ASK ", 9)) {
        /* 经 USART3 交给外置 ESP32-C3，由它直连百炼，回答原样回传 */
        char answer[320];
        if (ai_link_ask(line + 9, answer, sizeof(answer), 20000)) {
            char reply[400];
            snprintf(reply, sizeof(reply), "AI %s", answer);
            emit(reply);
        } else {
            emit("ERR reason=AI_FAILED");
        }
    } else if (!strcmp(line, "@WC1 ASKFIX")) {
        /* 内置中文问题（西安天气），走 UTF-8 常量，避开源码 GBK 编码问题 */
        char answer[320];
        if (ai_link_ask_weather(answer, sizeof(answer), 20000)) {
            char reply[400];
            snprintf(reply, sizeof(reply), "AI %s", answer);
            emit(reply);
        } else {
            emit("ERR reason=AI_FAILED");
        }
    } else if (!strcmp(line, "@WC1 DRAWFIX")) {
        /* 一键：切到对话页 -> 请 C3 渲染 -> 上传位图上屏。
         * 已停在 CHAT 页时不再请求切页（重复请求会被判为无效状态）。 */
        unsigned tries = 0;
        if (bitmap_active) { emit("ERR reason=DISPLAY_BUSY"); return; }
        page_flow_get_status(&status);
        if (strcmp(status.page, "CHAT")) {
            if (page_flow_request(1) <= 0) { emit("ERR reason=NOT_READY"); return; }
            do {
                vTaskDelay(pdMS_TO_TICKS(50));
                page_flow_get_status(&status);
            } while (strcmp(status.page, "CHAT") && ++tries < 60);
            if (strcmp(status.page, "CHAT")) { emit("ERR reason=NO_CHAT"); return; }
        }
        emit("ACK cmd=DRAWFIX stage=ask");
        if (!ai_link_ask_weather_bitmap(bitmap, sizeof(bitmap), 40000)) {
            snprintf(reply, sizeof(reply), "ERR reason=AI_FAILED detail=%s",
                     ai_link_last_error());
            emit(reply);
            return;
        }
        if (!page_flow_chat_bitmap(status.revision, bitmap)) {
            emit("ERR reason=STALE_CHAT"); return;
        }
        emit("ACK cmd=DRAWFIX stage=shown");
    } else if (!strcmp(line, "@WC1 WAKE") || !strcmp(line, "@WC1 HOME")) {
        int wake = !strcmp(line, "@WC1 WAKE");
        result = page_flow_request(wake);
        if (!result) emit("ERR reason=NOT_READY");
        else if (result < 0) emit("ERR reason=BUSY_OR_STATE");
        else emit(wake ? "ACK cmd=WAKE" : "ACK cmd=HOME");
    } else if (!strncmp(line, "@WC1", 4)) {
        emit("ERR reason=UNKNOWN_COMMAND");
    }
}

/* [阅读说明]
 * 处理 USART3 的 @WC1 请求，结束时发送 WCEND；OTAENTER 还需要检测 K3 才允许复位进入 Bootloader。
 */
static void peer_command(const char *line)
{
    if (strncmp(line, "@WC1 ", 5)) return;
    peer_response = true;
#ifdef WC_OTA_APP
    if (!strcmp(line, "@WC1 OTAVERIFY")) {
        const volatile uint32_t *record = (const volatile uint32_t *)WC_APP_RECORD;
        char response[80];
        snprintf(response, sizeof(response), "IMAGE size=%lu crc=%08lx",
                 (unsigned long)record[1], (unsigned long)record[2]);
        emit(response);
        ai_link_write("WCEND\r\n");
        peer_response = false;
        return;
    }
    if (!strcmp(line, "@WC1 OTAENTER")) {
        if (GPIOC->IDR & GPIO_Pin_5) {
            emit("ERR reason=HOLD_K3");
        } else {
            volatile uint32_t *cookie = (volatile uint32_t *)WC_COOKIE_ADDR;
            cookie[0] = WC_BOOT_COOKIE;
            cookie[1] = ~WC_BOOT_COOKIE;
            emit("ACK cmd=OTAENTER");
            ai_link_write("WCEND\r\n");
            NVIC_SystemReset();
        }
        ai_link_write("WCEND\r\n");
        peer_response = false;
        return;
    }
#endif
    if (!strcmp(line, "@WC1 HELLO") || !strcmp(line, "@WC1 STATUS") ||
        !strcmp(line, "@WC1 WAKE") || !strcmp(line, "@WC1 HOME") ||
        !strncmp(line, "@WC1 THINK ", 11) ||
        !strncmp(line, "@WC1 LCDSTART ", 14) ||
        !strncmp(line, "@WC1 LCDDATA ", 13) ||
        !strncmp(line, "@WC1 LCDEND ", 12))
        command(line);
    else
        emit("ERR reason=UNKNOWN_COMMAND");
    ai_link_write("WCEND\r\n");
    peer_response = false;
}

/* [阅读说明]
 * 优先级 5，每轮检查 USART3 和 USART1 缓冲，处理点阵超时，并报告状态及 K2 事件。
 * USART3 只由该任务消费，避免多个任务争抢同一个接收流。
 */
static void host_task(void *unused)
{
    char line[256];
    char peer_line[256];
    unsigned used = 0, discard = 0, previous_revision = ~0u;
    unsigned previous_key2 = 0;
    TickType_t last_report = 0;
    (void)unused;
    for (;;) {
        unsigned has_byte, bad;
        unsigned char byte = 0;
        page_flow_status_t status;
        TickType_t now;
        now = xTaskGetTickCount();
        if (bitmap_active && (TickType_t)(now - bitmap_activity) > pdMS_TO_TICKS(3000))
            bitmap_active = 0;
        if (ai_link_poll_command(peer_line, sizeof(peer_line)))
            peer_command(peer_line);
        do {
            taskENTER_CRITICAL();
            bad = overflow;
            overflow = 0;
            if (bad) tail = head;
            has_byte = head != tail;
            if (has_byte) { byte = rx[tail]; tail = (tail + 1) & 255; }
            taskEXIT_CRITICAL();
            if (bad) { used = 0; discard = 1; emit("ERR reason=RX_OVERFLOW"); }
            if (!has_byte) break;
            if (byte == '\n') {
                if (!discard && used) { line[used] = 0; command(line); }
                used = discard = 0;
            } else if (byte != '\r' && !discard) {
                if (used < sizeof(line) - 1 && byte >= 32 && byte < 127)
                    line[used++] = (char)byte;
                else { used = 0; discard = 1; emit("ERR reason=BAD_LINE"); }
            }
        } while (has_byte);
        now = xTaskGetTickCount();
        page_flow_get_status(&status);
        if (status.key2_count != previous_key2) {
            previous_key2 = status.key2_count;
            ai_link_write("WCKEY2\r\n");
        }
        if (status.revision != previous_revision ||
            (TickType_t)(now - last_report) >= pdMS_TO_TICKS(1000)) {
            report();
            previous_revision = status.revision;
            last_report = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void host_console_start(void)
{
    BaseType_t created;
    ai_link_init();
    console_received_register(receive_byte);
    created = xTaskCreate(host_task, "host", 768, NULL, 5, NULL);
    configASSERT(created == pdPASS);
}
