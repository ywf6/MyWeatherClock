/* [阅读说明]
 * 天气数据结构头文件：定义天气信息结构体和心知天气响应解析函数声明。
 * parse_seniverse_response() 从 ESP-AT 返回的 JSON 文本中提取城市、天气和温度。
 */
#ifndef __WEAHTER_H__
#define __WEAHTER_H__

#include <stdbool.h>
#include <stdint.h>

/* 天气信息结构体：解析心知天气 API 响应后填充 */
typedef struct
{
	char city[32];          /* 城市名，如 "西安" */
	char loaction[128];     /* 地理路径，如 "西安,西安,陕西,中国" */
	char weather[16];       /* 天气现象文本，如 "晴"、"多云" */
	int weather_code;       /* 天气代码，对应心知天气代码表 */
	float temperature;      /* 温度（摄氏度） */
} weather_info_t;

/* 解析心知天气 now.json 响应。
 * response: ESP-AT HTTP GET 返回的原始文本
 * info: 输出参数，解析成功后填充
 * 返回 true 表示成功提取到温度等关键字段 */
bool parse_seniverse_response(const char *response, weather_info_t *info);

#endif /* __WEAHTER_H__ */
