/* [阅读说明]
 * 字体系统头文件：定义字体结构体和所有外部字体常量声明。
 * 字体采用点阵格式，ASCII 字符宽度为字号一半，中文字符宽度等于字号。
 * ascii_map 是字符映射表（按出现顺序排列），ascii_model 是对应的点阵数据。
 * chinese 是中文字符表，通过名称匹配查找点阵。
 * 支持的字号：16/20/24/32/54/64/76 像素高度，多种粗细变体。
 */
#ifndef __FONT_H
#define __FONT_H

/* ASCII 可见字符集：从空格(0x20)到 ~(0x7E) */
// !"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~

#include <stdint.h>

/* 中文条目：名称 + 点阵数据指针 */
typedef struct
{
    const char *name;
    const uint8_t *model;
} font_chinese_t;

/* 字体描述结构体 */
typedef struct
{
    const uint8_t *ascii_model;     /* ASCII 点阵数据基地址 */
    const char *ascii_map;          /* ASCII 字符映射表，NULL 表示按 ' ' 起始顺序排列 */
    const font_chinese_t *chinese;  /* 中文字符表，以 NULL 结尾 */
    uint16_t size;                  /* 字号高度（像素），宽度为 size/2（ASCII）或 size（中文） */
} font_t;

/* 字体常量声明：按字号和粗细命名 */
extern const font_t font16_maple;               /* 16px 常规 */
extern const font_t font20_maple_bold;          /* 20px 粗体 */
extern const font_t font24_maple_semibold;      /* 24px 半粗 */
extern const font_t font24_maple_bold;          /* 24px 粗体 */
extern const font_t font32_maple_bold;          /* 32px 粗体 */
extern const font_t font54_maple_bold;          /* 54px 粗体 */
extern const font_t font54_maple_semibold;      /* 54px 半粗 */
extern const font_t font64_maple_extrabold;     /* 64px 特粗 */
extern const font_t font76_maple_extrabold;     /* 76px 特粗 */

#endif /* __FONT_H */
