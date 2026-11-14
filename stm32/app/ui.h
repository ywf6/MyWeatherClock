/* [阅读说明]
 * UI 消息队列接口头文件：页面函数通过这些接口提交绘制请求，由 UI 任务异步执行。
 * 设计模式为生产者-消费者：多个页面函数是生产者，单一 UI 任务是消费者。
 * 屏幕分辨率 240x320，颜色格式 RGB565。
 */
#ifndef __APP_UI_H__
#define __APP_UI_H__

#include <stdint.h>
#include "font.h"
#include "image.h"

/* 屏幕分辨率：宽 240 像素，高 320 像素 */
#define UI_WIDTH    240
#define UI_HEIGHT   320

/* RGB565 颜色宏：从 8 位 RGB 分量合成 16 位颜色值
 * r 取高 5 位，g 取高 6 位，b 取高 5 位 */
#define mkcolor(r, g, b) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

/* 初始化 UI 子系统：创建消息队列（容量 16）和 UI 任务（优先级 8，栈深 1024 字） */
void ui_init(void);

/* 矩形区域填充纯色。参数 width/height 实际作为右下角坐标使用（含边界端点） */
void ui_fill_color(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color);

/* 在指定位置绘制字符串。字符串会被复制到堆内存，绘制完成后由 UI 任务释放 */
void ui_write_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color, const font_t *font);

/* 在指定位置绘制图片。图片数据不复制，调用方须保证生命周期覆盖异步绘制 */
void ui_draw_image(uint16_t x, uint16_t y, const image_t *image);

/* 提交 216x208 的 1bpp 聊天位图（5616 字节）。数据会复制到堆，绘制后释放 */
int ui_draw_chat_bitmap(const uint8_t *bits);

#endif /* __APP_UI_H__ */
