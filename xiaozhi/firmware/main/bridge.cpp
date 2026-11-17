#include "probe.h"
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
static SemaphoreHandle_t mutex;
static char line[256];
static size_t used;
static bool discard;
static bool read_line(char*out) {
    uint8_t byte;
    while(uart_read_bytes(UART_NUM_1,&byte,1,0)==1) {
        if(byte=='\n') {
            bool ok=!discard&&used;line[used]=0;
            if(ok)memcpy(out,line,used+1);
            used=0;discard=false;if(ok)return true;
        } else if(byte!='\r'&&!discard) {
            if(used+1<sizeof(line))line[used++]=byte;
            else {used=0;discard=true;}
        }
    }
    return false;
}
bool bridge_exchange(const char*command,char*response,size_t capacity,int timeout_ms) {
    if(strlen(command)>245||!capacity||xSemaphoreTake(mutex,pdMS_TO_TICKS(25000))!=pdTRUE)return false;
    response[0]=0;char received[256];
    while(read_line(received))if(!strcmp(received,"WCKEY2"))key_event=true;
    uart_write_bytes(UART_NUM_1,command,strlen(command));uart_write_bytes(UART_NUM_1,"\r\n",2);
    int64_t deadline=esp_timer_get_time()+timeout_ms*1000LL;
    bool ended=false;size_t length=0;
    while(esp_timer_get_time()<deadline) {
        if(read_line(received)) {
            if(!strcmp(received,"WCKEY2")){key_event=true;continue;}
            if(!strcmp(received,"WCEND")){ended=true;break;}
            size_t count=strlen(received);if(length+count+2>=capacity)break;
            memcpy(response+length,received,count);length+=count;
            response[length++]='\n';response[length]=0;
        }else vTaskDelay(pdMS_TO_TICKS(1));
    }
    if(!ended){uart_flush_input(UART_NUM_1);used=0;discard=false;}
    xSemaphoreGive(mutex);
    return ended&&!strstr(response,"@WC1 ERR ");
}
static void keys(void*) {
    char received[256];
    for(;;) {
        if(xSemaphoreTake(mutex,0)==pdTRUE) {
            while(read_line(received))if(!strcmp(received,"WCKEY2"))key_event=true;
            xSemaphoreGive(mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void bridge_init() {
    mutex=xSemaphoreCreateMutex();configASSERT(mutex);
    uart_config_t config={};config.baud_rate=115200;config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;
    config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1,&config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1,0,1,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1,4096,0,0,nullptr,0));
    configASSERT(xTaskCreate(keys,"wc-keys",3072,nullptr,5,nullptr)==pdPASS);
}
