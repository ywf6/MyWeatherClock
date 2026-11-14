/* [阅读说明]
 * ST7789 屏幕驱动头文件：声明初始化和绘图接口。
 * 屏幕分辨率 240x320，颜色格式 RGB565。
 * SPI2 + DMA1 Stream4 像素传输，寄存器命令走轮询。
 * 坐标参数：fill_color 使用左上角+右下角坐标（含边界端点）。
 */
#ifndef __ST7789_H__
#define __ST7789_H__

#include <stdbool.h>
#include <stdint.h>
#include "font.h"
#include "image.h"

/* 屏幕分辨率 */
#define ST7789_WIDTH    240
#define ST7789_HEIGHT   320

/* 初始化屏幕：SPI2/DMA1 Stream4/NVIC + ST7789 寄存器序列 + 背光 */
void st7789_init(void);

/* 矩形区域填充纯色。参数为左上角(x1,y1)和右下角(x2,y2)坐标，含边界 */
void st7789_fill_color(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/* 在指定位置绘制字符串（支持 ASCII 和 GB2312 中文） */
void st7789_write_string(uint16_t x, uint16_t y, const char *str, uint16_t color, uint16_t bg_color, const font_t *font);

/* 在指定位置绘制 RGB565 图片 */
void st7789_draw_image(uint16_t x, uint16_t y, const image_t *image);

/* 绘制 216x208 的 1bpp 位图（用于 AI 对话页），逐行展开为 RGB565 */
void st7789_draw_chat_bitmap(const uint8_t *bits);

#endif /* __ST7789_H__ */
