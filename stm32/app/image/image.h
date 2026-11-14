/* [阅读说明]
 * 图片资源头文件：定义图片结构体和所有外部图片常量声明。
 * 图片格式为 RGB565（每像素 2 字节），数据按行优先排列。
 * img_* 为界面图片，icon_* 为天气/功能图标。
 * 图片数据存储在 Flash 中（const），不需要动态分配。
 */
#ifndef __IMAGE_H__
#define __IMAGE_H__

#include <stdint.h>

/* 图片描述结构体 */
typedef struct
{
    uint16_t width;     /* 图片宽度（像素） */
    uint16_t height;    /* 图片高度（像素） */
    const uint8_t *data;/* RGB565 像素数据，长度 = width * height * 2 字节 */
} image_t;

/* 界面图片 */
extern const image_t img_meihua;     /* 梅花 Logo，欢迎页显示 */
extern const image_t img_error;      /* 错误图标，错误页显示 */
extern const image_t img_wifi;       /* Wi-Fi 图标，连接页显示 */

/* 功能图标 */
extern const image_t icon_wenduji;   /* 温度计图标，主页室外区显示 */
extern const image_t icon_wifi;      /* Wi-Fi 小图标，主页状态栏显示 */

/* 天气图标：按心知天气代码映射 */
extern const image_t icon_duoyun;    /* 多云 */
extern const image_t icon_leizhenyu; /* 雷阵雨 */
extern const image_t icon_qing;      /* 晴 */
extern const image_t icon_yintian;   /* 阴天 */
extern const image_t icon_yueliang;  /* 月亮（夜间） */
extern const image_t icon_zhongxue;  /* 中雪 */
extern const image_t icon_zhongyu;   /* 中雨 */
extern const image_t icon_na;        /* 未知天气（扬沙、雾等） */

#endif /* __IMAGE_H__ */
