/* [阅读说明]
 * STM32 与外置 ESP32-C3 的 USART3 链路头文件：声明通信接口和行协议。
 *
 * 硬件连接：STM32(PD8=TX / PD9=RX) <-> 外置 ESP32-C3（PEER）
 *
 * 行协议（ASCII 头 + UTF-8 正文，CR/LF 结束）：
 *   本机发出 : "Q <问题>\r\n"        查询请求
 *   对端回答 : "A <答案>"             成功
 *              "E <原因>"             失败
 *   连通性   : 发 "P\r\n" 期望 "A pong"
 *   位图协议 : "G <序号> <hex>" 分块 -> "GEND <字节和>"
 *
 * 当前主链路由 host_console 任务消费 @WC1 命令；
 * 历史 Q/A/G 同步接口保留供 ASK/ASKFIX/DRAWFIX 诊断路径使用。
 */
#ifndef __AI_LINK_H__
#define __AI_LINK_H__

#include <stdbool.h>
#include <stdint.h>

/* 初始化 USART3（PD8/PD9, 115200 8N1）和接收中断 */
void ai_link_init(void);

/* 从 USART3 环形缓冲轮询一条完整命令行（LF 结束）。
 * 仅 host 任务调用，保证单一消费者。返回 true 表示取到完整行 */
bool ai_link_poll_command(char *line, unsigned size);

/* 轮询发送文本到 USART3（等待 TXE 逐字节发送） */
void ai_link_write(const char *text);

/* 发送问题并等待对端回答。成功返回 true 并把答案写入 out（含终止符）。
 * timeout_ms 是本机等待上限；对端约需 1~3 秒完成一次云端调用 */
bool ai_link_ask(const char *question, char *out, unsigned out_size, unsigned timeout_ms);

/* 发送内置的 UTF-8 中文问题（西安天气），供按键直接调用。
 * 固件源码是 GBK，此处用 UTF-8 字节常量避免编码混淆 */
bool ai_link_ask_weather(char *out, unsigned out_size, unsigned timeout_ms);

/* 提问并接收对端渲染好的 216x208 1bpp 位图（5616 字节）。
 * 协议：对端先回 "A <文字>"，再发 "G <序号> <hex>" 分块，最后 "GEND <字节和>"。
 * bitmap 由调用方提供，长度须 >= size */
bool ai_link_ask_bitmap(const char *question, uint8_t *bitmap, unsigned size,
                        unsigned timeout_ms);
bool ai_link_ask_weather_bitmap(uint8_t *bitmap, unsigned size, unsigned timeout_ms);

/* 最近一次失败原因（对端 "E xxx" 的 xxx，或本机判定的错误）。用于诊断 */
const char *ai_link_last_error(void);

/* 只做一次连通性探测（发送 P，期望 A pong） */
bool ai_link_ping(unsigned timeout_ms);

#endif /* __AI_LINK_H__ */
