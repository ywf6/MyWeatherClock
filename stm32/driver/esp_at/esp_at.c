/* [阅读说明]
 * 板载 ESP-AT 驱动：USART2(PA2/PA3)、115200，发送用 DMA1 Stream6，接收用 RXNE 中断。
 * 任务发送 AT 命令后等待应答信号量；中断按行识别 OK/ERROR/ready 并唤醒等待任务。
 * 共享接收缓冲没有互斥锁，当前主要依靠初始化阶段和单一 workqueue 消费者串行访问。
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stm32f4xx.h"
#include "esp_at.h"

#define ESP_AT_DEBUG    1

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

typedef enum
{
    AT_ACK_NONE,
    AT_ACK_OK,
    AT_ACK_ERROR,
    AT_ACK_BUSY,
} at_ack_t;

typedef struct
{
    at_ack_t ack;
    const char *string;
} at_ack_match_t;

static const at_ack_match_t at_ack_matches[] = 
{
    {AT_ACK_OK, "OK\r\n"},
    {AT_ACK_ERROR, "ERROR\r\n"},
    {AT_ACK_BUSY, "busy p…\r\n"},
};

static char *rxline;
static char rxbuf[1024];
static uint32_t rxlen;
static at_ack_t rxack;
static SemaphoreHandle_t at_ack_sempahore;

static bool esp_at_write_command(const char *command, uint32_t timeout);
static bool esp_at_wait_boot(uint32_t timeout);

static void esp_at_io_init(void)
{
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);
    
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_High_Speed;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
}

static void esp_at_usart_init(void)
{
    USART_InitTypeDef USART_InitStructure;
    USART_StructInit(&USART_InitStructure);

    USART_InitStructure.USART_BaudRate = 115200;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    
    USART_Init(USART2, &USART_InitStructure);
    USART_DMACmd(USART2, USART_DMAReq_Tx, ENABLE);
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART2, ENABLE);
}

static void esp_at_dma_init(void)
{
    DMA_InitTypeDef DMA_InitStruct;
    DMA_StructInit(&DMA_InitStruct);
    DMA_InitStruct.DMA_Channel = DMA_Channel_4;
    DMA_InitStruct.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DR;
    DMA_InitStruct.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    DMA_InitStruct.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStruct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStruct.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStruct.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    DMA_InitStruct.DMA_Mode = DMA_Mode_Normal;
    DMA_InitStruct.DMA_Priority = DMA_Priority_Medium;
    DMA_InitStruct.DMA_FIFOMode = DMA_FIFOMode_Enable;
    DMA_InitStruct.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;
    DMA_InitStruct.DMA_MemoryBurst = DMA_MemoryBurst_INC8;
    DMA_InitStruct.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(DMA1_Stream6, &DMA_InitStruct);
}

static void esp_at_int_init(void)
{
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = USART2_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    NVIC_SetPriority(USART2_IRQn, 5);
}

static void esp_at_lowlevel_init(void)
{
    esp_at_usart_init();
    esp_at_dma_init();
    esp_at_int_init();
    esp_at_io_init();
}

bool esp_at_init(void)
{
    if (!at_ack_sempahore) {
        at_ack_sempahore = xSemaphoreCreateBinary();
        configASSERT(at_ack_sempahore);
        rxline = rxbuf;
        esp_at_lowlevel_init();
    }

    return esp_at_wait_boot(3000);
}

/* [阅读说明]
 * 启动发送 DMA 后立即返回；这里没有用 DMA 完成中断唤醒任务，后续等待的是 AT 应答。
 */
static void esp_at_usart_write(const char *data)
{
    uint32_t len = strlen(data);
    
    DMA1_Stream6->M0AR = (uint32_t)data;
    DMA1_Stream6->NDTR = len;

    DMA_ClearFlag(DMA1_Stream6, DMA_FLAG_TCIF6);
    DMA_Cmd(DMA1_Stream6, ENABLE);
}

static at_ack_t match_internal_ack(const char *str)
{
    for (uint32_t i = 0; i < ARRAY_SIZE(at_ack_matches); i++)
    {
        if (strcmp(str, at_ack_matches[i].string) == 0)
            return at_ack_matches[i].ack;
    }
    
    return AT_ACK_NONE;
}

/* [阅读说明]
 * 重置本次接收状态并等待应答信号量；超时返回 AT_ACK_NONE，让上层决定是否重试。
 */
static at_ack_t esp_at_usart_wait_receive(uint32_t timeout)
{
    rxlen = 0;
    rxline = rxbuf;
    
    bool acked = xSemaphoreTake(at_ack_sempahore, pdMS_TO_TICKS(timeout)) == pdPASS;
    return acked ? rxack : AT_ACK_NONE;
}

/* [阅读说明]
 * 发送命令并等待终结应答；超时期间任务阻塞，其他就绪任务可运行。
 * 现状提醒：DEBUG 会输出完整 AT 命令，配置 Wi-Fi/天气密钥后调试日志可能包含凭据。
 */
static bool esp_at_write_command(const char *command, uint32_t timeout)
{
#if ESP_AT_DEBUG
    const bool log_command = strncmp(command, "AT+CWJAP=", 9) != 0 && strncmp(command, "AT+HTTPCLIENT=", 14) != 0;
    if (log_command) printf("[DEBUG] Send: %s\n", command);
#endif

    esp_at_usart_write(command);
    at_ack_t ack = esp_at_usart_wait_receive(timeout);

#if ESP_AT_DEBUG
    if (log_command) printf("[DEBUG] Response:\n%s\n", rxbuf);
#endif

    return ack == AT_ACK_OK;
}

static const char *esp_at_get_response(void)
{
    return rxbuf;
}

static bool esp_at_wait_boot(uint32_t timeout)
{
    for (int t = 0; t < timeout; t += 100)
    {
        if (esp_at_write_command("AT\r\n", 100))
            return true;
    }
    
    return false;
}

bool esp_at_wifi_init(void)
{
    return esp_at_write_command("AT+CWMODE=1\r\n", 2000);
}

bool esp_at_connect_wifi(const char *ssid, const char *pwd, const char *mac)
{
    if (ssid == NULL && pwd == NULL)
        return esp_at_write_command("AT+CWJAP\r\n", 17000);
    if (ssid == NULL || pwd == NULL)
        return false;

    char cmd[256];
    int len = mac
        ? snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\",\"%s\"\r\n", ssid, pwd, mac)
        : snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    if (len < 0 || (size_t)len >= sizeof(cmd))
        return false;

    return esp_at_write_command("AT+SYSSTORE=1\r\n", 2000) &&
           esp_at_write_command(cmd, 17000);
}

static bool parse_cwstate_response(const char *response, esp_wifi_info_t *info)
{
//    AT+CWSTATE?
//    +CWSTATE:2,"Xiaomi Mi MIX 3_5577"

//    OK
	response = strstr(response, "+CWSTATE:");
	if (response == NULL)
		return false;
	
	int wifi_state;
	if (sscanf(response, "+CWSTATE:%d", &wifi_state) != 1)
		return false;
	
	info->connected = (wifi_state == 2);
	
	return true;
}

static bool parse_cwjap_response(const char *response, esp_wifi_info_t *info)
{
//    AT+CWJAP?
//    +CWJAP:"Xiaomi Mi MIX 3_5577","da:b5:3a:e3:2f:60",9,-48,0,1,3,0,1

//    OK
	response = strstr(response, "+CWJAP:");
	if (response == NULL)
		return false;
	
	if (sscanf(response, "+CWJAP:\"%63[^\"]\",\"%17[^\"]\",%d,%d", info->ssid, info->bssid, &info->channel, &info->rssi) != 4)
		return false;
	
	return true;
}

bool esp_at_get_wifi_info(esp_wifi_info_t *info)
{
    if (!esp_at_write_command("AT+CWSTATE?\r\n", 2000))
        return false;
    
    if (!parse_cwstate_response(esp_at_get_response(), info))
        return false;
    
    if (info->connected == true)
    {
        if (!esp_at_write_command("AT+CWJAP?\r\n", 2000))
            return false;
        
        if (!parse_cwjap_response(esp_at_get_response(), info))
            return false;
    }
    
    return true;
}

bool wifi_is_connected(void)
{
    esp_wifi_info_t info;
    if (esp_at_get_wifi_info(&info))
    {
        return info.connected;
    }
    return false;
}

/* [阅读说明]
 * 配置 SNTP 时区为 UTC+8；app.c 周期读取网络时间，再同步到本地 RTC。
 */
bool esp_at_sntp_init(void)
{
    if (!esp_at_write_command("AT+CIPSNTPCFG=1,8\r\n", 2000))
        return false;
    
    return true;
}

static uint8_t month_str_to_num(const char *month_str)
{
	const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", 
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	for (uint8_t i = 0; i < 12; i++)
	{
		if (strcmp(month_str, months[i]) == 0)
		{
			return i + 1;
		}
	}
	return 0;
}


static uint8_t weekday_str_to_num(const char *weekday_str)
{
	const char *weekdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
	for (uint8_t i = 0; i < 7; i++) {
		if (strcmp(weekday_str, weekdays[i]) == 0)
		{
			return i + 1;
		}
	}
	return 0;
}

/* [阅读说明]
 * 解析 AT 返回的时间文本。
 * 现状提醒：strstr 未找到标记时会返回 NULL，当前代码未检查便交给 sscanf。
 */
static bool parse_cipsntptime_response(const char *response, esp_date_time_t *date)
{
//	AT+CIPSNTPTIME?
//	+CIPSNTPTIME:Sun Jul 27 14:07:19 2025
//	OK
	char weekday_str[8];
	char month_str[4];
	response = strstr(response, "+CIPSNTPTIME:");
	if (sscanf(response, "+CIPSNTPTIME:%3s %3s %hhu %hhu:%hhu:%hhu %hu", 
			   weekday_str, month_str, 
			   &date->day, &date->hour, &date->minute, &date->second, &date->year) != 7)
		return false;
	
	date->weekday = weekday_str_to_num(weekday_str);
	date->month = month_str_to_num(month_str);
	
	return true;
}

bool esp_at_sntp_get_time(esp_date_time_t *date)
{
    if (!esp_at_write_command("AT+CIPSNTPTIME?\r\n", 2000))
        return false;
    
    if (!parse_cipsntptime_response(esp_at_get_response(), date))
        return false;
    
    return true;
}

/* [阅读说明]
 * HTTP 命令发送缓冲和接收缓冲共用 rxbuf；返回指针也是内部缓冲，后续 AT 操作会覆盖它。
 * 上层应在下一次 AT 命令前完成解析。
 */
const char *esp_at_http_get(const char *url)
{
//    AT+HTTPCLIENT=2,1,"https://api.seniverse.com/v3/weather/now.json?key=YOUR_SENIVERSE_KEY&location=WTEMH46Z5N09&language=en&unit=c",,,2
//    +HTTPCLIENT:261,{"results":[{"location":{"id":"WTEMH46Z5N09","name":"Hefei","country":"CN","path":"Hefei,Hefei,Anhui,China","timezone":"Asia/Shanghai","timezone_offset":"+08:00"},"now":{"text":"Cloudy","code":"4","temperature":"32"},"last_update":"2025-07-26T16:30:00+08:00"}]}

//    OK
    char *txbuf = rxbuf;
    snprintf(txbuf, sizeof(rxbuf), "AT+HTTPCLIENT=2,1,\"%s\",,,2\r\n", url);
    bool ret = esp_at_write_command(txbuf, 5000);
    return ret ? esp_at_get_response() : NULL;
}

/* [阅读说明]
 * 按 LF 分行匹配 AT 终结应答；只在匹配时释放信号量，不在中断里执行 HTTP 业务。
 * 现状提醒：FromISR 的唤醒标志尚未显式初始化，缓冲满时的丢弃/恢复逻辑也需后续完善。
 */
void USART2_IRQHandler(void)
{    
    if (USART_GetITStatus(USART2, USART_IT_RXNE) == SET)
    {
        if (rxlen < sizeof(rxbuf) - 1)
        {
            rxbuf[rxlen++] = USART_ReceiveData(USART2);
            if (rxbuf[rxlen - 1] == '\n')
            {
                rxbuf[rxlen] = '\0';
                at_ack_t ack = match_internal_ack(rxline);
                if (ack != AT_ACK_NONE)
                {
                    rxack = ack;
                    BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
                    xSemaphoreGiveFromISR(at_ack_sempahore, &pxHigherPriorityTaskWoken);
                    portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);
                }
                rxline = rxbuf + rxlen;
            }
        }

        USART_ClearITPendingBit(USART2, USART_IT_RXNE);
    }
}
