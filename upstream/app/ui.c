/* [阅读说明]
 * UI 执行层：页面函数提交消息，唯一 UI 任务依次调用 ST7789 驱动。
 * 队列复制消息结构体，但结构体里的指针仍指向原数据，因此要区分动态副本和静态资源。
 * 当前队列容量 16，UI 任务优先级 8；实际 SPI/DMA 传输详见 driver/st7789/st7789.c。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "st7789.h"
#include "ui.h"
#include "font.h"
#include "image.h"

typedef enum
{
    UI_ACTION_FILL_COLOR,
    UI_ACTION_WRITE_STRING,
    UI_ACTION_DRAW_IMAGE,
    UI_ACTION_CHAT_BITMAP,
} ui_action_t;

typedef struct
{
    ui_action_t action;
    union
    {
        struct
        {
            uint16_t x;
            uint16_t y;
            uint16_t width;
            uint16_t height;
            uint16_t color;
        } fill_color;
        struct
        {
            uint16_t x;
            uint16_t y;
            const char *str;
            uint16_t color;
            uint16_t bg_color;
            const font_t *font;
        } write_string;
        struct
        {
            uint16_t x;
            uint16_t y;
            const image_t *image;
        } draw_image;
    };
} ui_message_t;

static QueueHandle_t ui_queue;

/* [阅读说明]
 * 本任务初始化并操作屏幕；无消息时阻塞，动态文本和聊天点阵在绘制后释放。
 */
static void ui_func(void *param)
{
    ui_message_t msg;
    
    st7789_init();
    
    while (1)
    {
        xQueueReceive(ui_queue, &msg, portMAX_DELAY);
        
        switch (msg.action)
        {
        case UI_ACTION_CHAT_BITMAP:
            st7789_draw_chat_bitmap((const uint8_t *)msg.draw_image.image);
            vPortFree((void *)msg.draw_image.image);
            break;
        case UI_ACTION_FILL_COLOR:
            st7789_fill_color(msg.fill_color.x, msg.fill_color.y,
                              msg.fill_color.width, msg.fill_color.height,
                              msg.fill_color.color);
            break;
        case UI_ACTION_WRITE_STRING:
            st7789_write_string(msg.write_string.x, msg.write_string.y,
                                msg.write_string.str,
                                msg.write_string.color, msg.write_string.bg_color,
                                msg.write_string.font);
            vPortFree((void*)msg.write_string.str);
            break;
        case UI_ACTION_DRAW_IMAGE:
            st7789_draw_image(msg.draw_image.x, msg.draw_image.y,
                              msg.draw_image.image);
            break;
        default:
            printf("Unknown UI action: %d\n", msg.action);
            break;
        }
    }
}

void ui_init(void)
{
    ui_queue = xQueueCreate(16, sizeof(ui_message_t));
    configASSERT(ui_queue);
    xTaskCreate(ui_func, "ui", 1024, NULL, 8, NULL);
}

/* [阅读说明]
 * 参数沿用历史命名 width/height，但驱动将其作为右下角 x2/y2 坐标，包含边界端点。
 */
void ui_fill_color(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color)
{
    ui_message_t msg;
    msg.action = UI_ACTION_FILL_COLOR;
    msg.fill_color.x = x;
    msg.fill_color.y = y;
    msg.fill_color.width = width;
    msg.fill_color.height = height;
    msg.fill_color.color = color;
    
    xQueueSend(ui_queue, &msg, portMAX_DELAY);
}

/* [阅读说明]
 * 先复制字符串到 FreeRTOS 堆，再将指针入队，避免调用方栈变量在异步绘制前失效。
 */
void ui_write_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color, const font_t *font)
{
    char *pstr = pvPortMalloc(strlen(str) + 1);
    if (pstr == NULL)
    {
        printf("ui write string malloc failed: %s", str);
        return;
    }
    strcpy(pstr, str);
    
    ui_message_t msg;
    msg.action = UI_ACTION_WRITE_STRING;
    msg.write_string.x = x;
    msg.write_string.y = y;
    msg.write_string.str = pstr;
    msg.write_string.color = color;
    msg.write_string.bg_color = bg_color;
    msg.write_string.font = font;
    
    xQueueSend(ui_queue, &msg, portMAX_DELAY);
}

/* [阅读说明]
 * 图片内容不复制；当前调用者使用长期存在的静态 image_t 资源。
 */
void ui_draw_image(uint16_t x, uint16_t y, const image_t *image)
{
    ui_message_t msg;
    msg.action = UI_ACTION_DRAW_IMAGE;
    msg.draw_image.x = x;
    msg.draw_image.y = y;
    msg.draw_image.image = image;
    
    xQueueSend(ui_queue, &msg, portMAX_DELAY);
}

/* [阅读说明]
 * 216 * 208 / 8 = 5616 字节，复制为独立缓冲区交给 UI 任务；绘制结束由消费者释放。
 */
int ui_draw_chat_bitmap(const uint8_t *bits)
{
    ui_message_t msg;
    uint8_t *copy = pvPortMalloc(5616);
    if (!copy) return 0;
    memcpy(copy, bits, 5616);
    msg.action = UI_ACTION_CHAT_BITMAP;
    msg.draw_image.image = (const image_t *)copy;
    xQueueSend(ui_queue, &msg, portMAX_DELAY);
    return 1;
}
