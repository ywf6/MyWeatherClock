/* [阅读说明]
 * 心知天气响应解析：从 AT 返回文本中提取城市、天气代码和温度，供 app.c 发布到页面。
 * 这是针对预期字段的字符串查找器，不是完整 JSON 解析器。
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "weather.h"

/*
 * 从 JSON 中取出 key 对应的字符串值，兼容冒号前后有无空白的两种格式：
 *   "name": "Hefei"   （旧格式，官方原代码假定）
 *   "name":"西安"      （新格式，心知接口现在返回）
 * 找不到 key、值不是字符串或缓冲区不足时返回 false。
 */
static bool json_find_string(const char *json, const char *key, char *out, size_t out_size)
{
	char pattern[16];
	if (json == NULL || out == NULL || out_size == 0)
		return false;

	if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern))
		return false;

	const char *p = strstr(json, pattern);
	if (p == NULL)
		return false;

	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p != ':')
		return false;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p != '"')
		return false;
	p++;

	size_t i = 0;
	while (*p != '\0' && *p != '"' && (i + 1) < out_size)
		out[i++] = *p++;
	out[i] = '\0';

	return i > 0;
}

/* [阅读说明]
 * 结构标记 results/now 当前要求冒号紧跟引号；字段字符串的查找支持部分空白。
 * 温度用 atof 转换，未严格验证数值格式；字符串转义、截断等边界也需要另行完善。
 */
bool parse_seniverse_response(const char *response, weather_info_t *info)
{
	if (response == NULL || info == NULL)
		return false;

	/* 只有命中业务字段才认为响应结构正确 */
	if (strstr(response, "\"results\":") == NULL)
		return false;
	if (strstr(response, "\"now\":") == NULL)
		return false;

	char value[32];

	/* 城市名（location.name），失败保留原值 */
	(void)json_find_string(response, "name", info->city, sizeof(info->city));

	/* 地理路径（location.path） */
	(void)json_find_string(response, "path", info->loaction, sizeof(info->loaction));

	/* 天气现象文本（now.text） */
	if (json_find_string(response, "text", value, sizeof(value)))
	{
		size_t len = strlen(value);
		if (len >= sizeof(info->weather))
			len = sizeof(info->weather) - 1;
		memcpy(info->weather, value, len);
		info->weather[len] = '\0';
	}

	/* 天气代码（now.code，字符串形式） */
	if (json_find_string(response, "code", value, sizeof(value)))
		info->weather_code = atoi(value);
	else
		info->weather_code = -1;

	/* 温度（now.temperature，字符串形式）。缺失则整体判失败，避免显示 0 度 */
	char temp_str[16];
	if (!json_find_string(response, "temperature", temp_str, sizeof(temp_str)))
		return false;

	info->temperature = (float)atof(temp_str);

	return true;
}
