/* [阅读说明]
 * STM32 与外置语音 C3 的 USART3 链路：PD8=TX、PD9=RX，115200 8N1。
 * 当前主链路由 host_console 任务消费 @WC1 命令，接收中断仅写环形缓冲。
 * 本文件也保留历史 Q/A/G 位图接口；当前 C3 固件的 Qt 主链路不使用这些接口调用模型。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include "ai_link.h"

/* USART3: PD8 = TX, PD9 = RX（AF7），115200 8N1。
 * 引脚来自 MHEVK V1.2.6 原理图第 1/4 页：PD8/PD9 为 USART3，
 * 经 33 ohm 串阻引到 U7 扩展排针第 18 / 17 脚，板上无其他负载。 */

#define AI_LINK_USART     USART3
#define AI_LINK_BAUD      115200u
#define AI_LINK_RX_SIZE   1024u

static volatile unsigned rx_head, rx_tail, rx_overflow;
static volatile uint8_t rx_buf[AI_LINK_RX_SIZE];

static char last_error[48] = "none";

/* 单调用者（host 任务）共用，避免把 1KB 行缓冲压到 3KB 的任务栈上。 */
static char line_buf[AI_LINK_RX_SIZE];

static void set_error(const char *text)
{
    unsigned i = 0;
    while (text[i] && i + 1 < sizeof(last_error))
    {
        last_error[i] = text[i];
        ++i;
    }
    last_error[i] = '\0';
}

const char *ai_link_last_error(void)
{
    return last_error;
}

static void ai_link_gpio_init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);

    GPIO_PinAFConfig(GPIOD, GPIO_PinSource8, GPIO_AF_USART3);
    GPIO_PinAFConfig(GPIOD, GPIO_PinSource9, GPIO_AF_USART3);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_High_Speed;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;      /* 对端未接时不悬空 */
    gpio.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9;
    GPIO_Init(GPIOD, &gpio);
}

static void ai_link_usart_init(void)
{
    USART_InitTypeDef uart;
    NVIC_InitTypeDef nvic;

    USART_StructInit(&uart);
    uart.USART_BaudRate = AI_LINK_BAUD;
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    uart.USART_Parity = USART_Parity_No;
    uart.USART_StopBits = USART_StopBits_1;
    uart.USART_WordLength = USART_WordLength_8b;
    USART_Init(AI_LINK_USART, &uart);

    USART_ITConfig(AI_LINK_USART, USART_IT_RXNE, ENABLE);
    USART_Cmd(AI_LINK_USART, ENABLE);

    nvic.NVIC_IRQChannel = USART3_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 6;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

void ai_link_init(void)
{
    rx_head = rx_tail = rx_overflow = 0;
    ai_link_gpio_init();
    ai_link_usart_init();
}

/* [阅读说明]
 * 中断生产字节，任务消费字节；环形缓冲满时标记 overflow，后续丢弃受损行并等待重新对齐。
 */
void USART3_IRQHandler(void)
{
    if (USART_GetITStatus(AI_LINK_USART, USART_IT_RXNE) != RESET)
    {
        uint8_t byte = (uint8_t)USART_ReceiveData(AI_LINK_USART);
        BaseType_t woken = pdFALSE;
        unsigned next = (rx_head + 1) % AI_LINK_RX_SIZE;
        if (next == rx_tail)
        {
            rx_overflow = 1;
        }
        else
        {
            rx_buf[rx_head] = byte;
            rx_head = next;
        }
        (void)woken;
        USART_ClearITPendingBit(AI_LINK_USART, USART_IT_RXNE);
    }
}

static void rx_drain(void)
{
    taskENTER_CRITICAL();
    rx_tail = rx_head;
    rx_overflow = 0;
    taskEXIT_CRITICAL();
}

/* [阅读说明]
 * 当前使用轮询发送，等待 TXE 后写字节，最后等 TC；这一通道没有采用 DMA 发送。
 */
void ai_link_write(const char *text)
{
    while (*text)
    {
        while (USART_GetFlagStatus(AI_LINK_USART, USART_FLAG_TXE) == RESET)
            ;
        USART_SendData(AI_LINK_USART, (uint8_t)*text++);
    }
    while (USART_GetFlagStatus(AI_LINK_USART, USART_FLAG_TC) == RESET)
        ;
}

/* [阅读说明]
 * 每次最多处理 256 字节，以 LF 组帧；接受可打印 ASCII，UTF-8 正文不走当前主机命令路径。
 */
bool ai_link_poll_command(char *line, unsigned size)
{
    static unsigned used, discard;
    for (unsigned budget = 0; budget < 256; ++budget)
    {
        uint8_t byte;
        taskENTER_CRITICAL();
        if (rx_overflow)
        {
            rx_tail = rx_head;
            rx_overflow = 0;
            used = 0;
            discard = 1;
        }
        if (rx_head == rx_tail)
        {
            taskEXIT_CRITICAL();
            break;
        }
        byte = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % AI_LINK_RX_SIZE;
        taskEXIT_CRITICAL();
        if (byte == '\n')
        {
            bool complete = !discard && used != 0;
            line[used] = '\0';
            used = discard = 0;
            if (complete) return true;
        }
        else if (byte != '\r' && !discard)
        {
            if (byte >= 32 && byte < 127 && used + 1 < size)
                line[used++] = (char)byte;
            else
                discard = 1;
        }
    }
    return false;
}

/* 从环形缓冲取一行（不含 CR/LF）。返回 1 取到完整行，0 超时。 */
/* [阅读说明]
 * 历史同步问答接口使用的超时读取函数；与 poll_command 共用接收缓冲，应保持单一消费者。
 */
static int read_line(char *line, unsigned size, TickType_t deadline)
{
    unsigned used = 0;
    for (;;)
    {
        uint8_t byte;
        unsigned has_byte;

        taskENTER_CRITICAL();
        has_byte = (rx_head != rx_tail);
        if (has_byte)
        {
            byte = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) % AI_LINK_RX_SIZE;
        }
        taskEXIT_CRITICAL();

        if (has_byte)
        {
            if (byte == '\n')
            {
                if (used == 0)
                    continue;              /* 忽略空行 */
                line[used] = '\0';
                return 1;
            }
            if (byte != '\r')
            {
                if (used + 1 < size)
                    line[used++] = (char)byte;
                else
                    return -1;             /* 过长，丢弃 */
            }
            continue;
        }

        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0)
            return 0;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static bool ask_internal(const char *request, char *out, unsigned out_size, unsigned timeout_ms)
{
    TickType_t deadline;

    rx_drain();
    ai_link_write(request);
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        int got = read_line(line_buf, sizeof(line_buf), deadline);
        if (got <= 0)
        {
            set_error(got == 0 ? "timeout" : "overlong");
            return false;
        }
        if (line_buf[0] == 'A' && line_buf[1] == ' ')
        {
            size_t len = strlen(line_buf + 2);
            if (len >= out_size)
                len = out_size - 1;
            memcpy(out, line_buf + 2, len);
            out[len] = '\0';
            return true;
        }
        if (line_buf[0] == 'E')
        {
            set_error(line_buf[1] ? line_buf + 2 : "peer");
            return false;
        }
        /* 其他行（对端调试输出）忽略，继续等 */
    }
}

bool ai_link_ask(const char *question, char *out, unsigned out_size, unsigned timeout_ms)
{
    char request[256];
    if (question == NULL || question[0] == '\0' || out == NULL || out_size < 2)
        return false;
    if (strlen(question) > 120)
        return false;
    snprintf(request, sizeof(request), "\r\nQ %s\r\n", question);
    return ask_internal(request, out, out_size, timeout_ms);
}

bool ai_link_ping(unsigned timeout_ms)
{
    char out[16];
    if (!ask_internal("P\r\n", out, sizeof(out), timeout_ms))
        return false;
    return strncmp(out, "pong", 4) == 0;
}

/* 「西安现在天气怎么样？出门要带伞吗？」的 UTF-8 字节。
 * 源文件是 GBK，中文直接写成字面量会变成 GBK 字节发给对端，故用转义。 */
static const char kQuestionWeather[] =
    "\xE8\xA5\xBF\xE5\xAE\x89\xE7\x8E\xB0\xE5\x9C\xA8\xE5\xA4\xA9\xE6\xB0\x94"
    "\xE6\x80\x8E\xE4\xB9\x88\xE6\xA0\xB7\xEF\xBC\x9F\xE5\x87\xBA\xE9\x97\xA8"
    "\xE8\xA6\x81\xE5\xB8\xA6\xE4\xBC\x9E\xE5\x90\x97\xEF\xBC\x9F";

bool ai_link_ask_weather(char *out, unsigned out_size, unsigned timeout_ms)
{
    char request[256];
    if (out == NULL || out_size < 2)
        return false;
    snprintf(request, sizeof(request), "\r\nQ %s\r\n", kQuestionWeather);
    return ask_internal(request, out, out_size, timeout_ms);
}

/* ---- 位图通道 ---- */

#define AI_BITMAP_CHUNK 128u

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 对端在同一行先回文字答案，再发位图分块。
 * 返回 true 表示收到 GEND 且校验和一致。 */
/* [阅读说明]
 * 历史 G/GEND 点阵协议：接收十六进制块并检查字节和。
 * 当前 Qt 主链路采用 host_console.c 的 LCDSTART/LCDATA/LCDEND 协议。
 */
static bool ask_bitmap_internal(const char *request, uint8_t *bitmap, unsigned size,
                                unsigned timeout_ms)
{
    unsigned received = 0, actual = 0, i, chunks = 0;
    TickType_t deadline;

    rx_drain();
    ai_link_write(request);
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        int got = read_line(line_buf, sizeof(line_buf), deadline);
        if (got <= 0)
        {
            char tmp[48];
            snprintf(tmp, sizeof(tmp), "%s ch=%u recv=%u", got == 0 ? "timeout" : "overlong",
                     chunks, received);
            set_error(tmp);
            return false;
        }

        if (line_buf[0] == 'A' && line_buf[1] == ' ')
        {
            continue;                  /* 文字答案，位图随后就到 */
        }
        if (line_buf[0] == 'E')
        {
            set_error(line_buf[1] ? line_buf + 2 : "peer");
            return false;
        }

        if (line_buf[0] == 'G' && line_buf[1] == ' ')
        {
            unsigned index = 0, offset;
            char *hex = line_buf + 2;
            unsigned hex_len;
            while (*hex >= '0' && *hex <= '9')
                index = index * 10 + (unsigned)(*hex++ - '0');
            if (*hex == ' ')
                hex++;
            hex_len = strlen(hex);
            offset = index * AI_BITMAP_CHUNK;
            if (hex_len == 0 || (hex_len & 1) || offset + hex_len / 2 > size)
            {
                set_error("badchunk");
                return false;
            }
            for (i = 0; i < hex_len; i += 2)
            {
                int hi = hex_val(hex[i]), lo = hex_val(hex[i + 1]);
                if (hi < 0 || lo < 0)
                {
                    set_error("badhex");
                    return false;
                }
                bitmap[offset + i / 2] = (uint8_t)((hi << 4) | lo);
            }
            if (offset + hex_len / 2 > received)
                received = offset + hex_len / 2;
            ++chunks;
            continue;
        }
        if (!strncmp(line_buf, "GEND ", 5))
        {
            unsigned expect = 0;
            const char *p = line_buf + 5;
            while (*p >= '0' && *p <= '9')
                expect = expect * 10 + (unsigned)(*p++ - '0');
            if (received != size)
            {
                set_error("short");
                return false;
            }
            for (i = 0; i < size; ++i)
                actual += bitmap[i];
            if ((actual & 0xFFFF) != expect)
            {
                set_error("checksum");
                return false;
            }
            return true;
        }
        /* 其他行忽略 */
    }
}

bool ai_link_ask_bitmap(const char *question, uint8_t *bitmap, unsigned size,
                        unsigned timeout_ms)
{
    char request[256];
    if (question == NULL || question[0] == '\0' || bitmap == NULL || size == 0)
        return false;
    if (strlen(question) > 120)
        return false;
    snprintf(request, sizeof(request), "\r\nQ %s\r\n", question);
    return ask_bitmap_internal(request, bitmap, size, timeout_ms);
}

bool ai_link_ask_weather_bitmap(uint8_t *bitmap, unsigned size, unsigned timeout_ms)
{
    char request[256];
    if (bitmap == NULL || size == 0)
        return false;
    snprintf(request, sizeof(request), "\r\nQ %s\r\n", kQuestionWeather);
    return ask_bitmap_internal(request, bitmap, size, timeout_ms);
}
