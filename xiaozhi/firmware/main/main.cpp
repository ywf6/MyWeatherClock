#include "probe.h"
#include "probe_config.h"
#include "../../../esp32/main/network_settings.h"
#include "../../../esp32/main/discovery_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "cJSON.h"
static const char*TAG="wc-xiaozhi";
std::atomic<Phase> phase{Phase::Idle};
std::atomic<bool> key_event{false},maintenance{false};
std::atomic<unsigned> audio_generation{0};
esp_websocket_client_handle_t websocket;
char session_id[65];
unsigned chat_revision;
static std::atomic<bool> ready{false};
static QueueHandle_t messages;
static uint8_t mac[6];
static char device_id[18],headers[256],client_id[40];
struct Message {uint16_t length;uint8_t opcode;char data[1536];};
static const char*str(const cJSON*o,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:"";}
static int number(const cJSON*o,const char*k){const cJSON*v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?v->valueint:0;}
static bool same(const cJSON*o,const char*k,const char*v){return !strcmp(str(o,k),v);}
bool send_text(const char*text) {
    return websocket&&esp_websocket_client_is_connected(websocket)&&
           esp_websocket_client_send_text(websocket,text,strlen(text),pdMS_TO_TICKS(1500))>=0;
}
static void send_json(cJSON*json){char*text=cJSON_PrintUnformatted(json);if(text){send_text(text);free(text);}cJSON_Delete(json);}
static cJSON*event(const char*type){cJSON*o=cJSON_CreateObject();cJSON_AddStringToObject(o,"type",type);cJSON_AddStringToObject(o,"session_id",session_id);return o;}
static void abort_turn() {
    audio_clear();phase=Phase::Idle;
    if(ready){cJSON*o=event("abort");cJSON_AddStringToObject(o,"reason","user_cancelled");send_json(o);}
}
void stop_recording() {
    Phase expected=Phase::Recording;
    if(!phase.compare_exchange_strong(expected,Phase::Waiting))return;
    cJSON*o=event("listen");cJSON_AddStringToObject(o,"state","stop");send_json(o);
}
static bool current_chat() {
    char response[1024];
    if(!bridge_exchange("@WC1 STATUS",response,sizeof(response),400))return true;
    const char*rev=strstr(response," rev=");
    return strstr(response,"page=CHAT ")&&rev&&strtoul(rev+5,nullptr,10)==chat_revision;
}
static void begin_turn() {
    if(!ready||maintenance){ESP_LOGW(TAG,"Gateway not ready or maintenance active");return;}
    if(phase==Phase::Recording){audio_request_stop();return;}
    abort_turn();
    char response[1024];
    const char*rev=nullptr;
    int64_t deadline=esp_timer_get_time()+4000000;
    while(!maintenance&&ready&&esp_timer_get_time()<deadline) {
        if(bridge_exchange("@WC1 STATUS",response,sizeof(response),500)) {
            if(strstr(response,"page=HOME "))return;
            if(strstr(response,"page=CHAT ")) {
                rev=strstr(response," rev=");
                if(rev)break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if(!rev||maintenance||!ready) {
        ESP_LOGW(TAG,"STM32 chat page not confirmed");
        return;
    }
    chat_revision=strtoul(rev+5,nullptr,10);
    char command[64];snprintf(command,sizeof(command),"@WC1 THINK %u",chat_revision);
    bridge_exchange(command,response,sizeof(response));
    cJSON*o=event("listen");cJSON_AddStringToObject(o,"state","start");cJSON_AddStringToObject(o,"mode","manual");
    cJSON_AddNumberToObject(o,"wc_revision",chat_revision);
    send_json(o);phase=Phase::Recording;audio_capture_start();
    ESP_LOGI(TAG,"Recording: K2 stops, maximum 6 seconds; K3 cancels");
}
static void mcp(const cJSON*request) {
    const cJSON*p=cJSON_GetObjectItemCaseSensitive(request,"payload");
    const cJSON*id=cJSON_GetObjectItemCaseSensitive(p,"id");
    if(!id)return;
    cJSON*reply=event("mcp");cJSON*body=cJSON_AddObjectToObject(reply,"payload");
    cJSON_AddStringToObject(body,"jsonrpc","2.0");cJSON_AddItemToObject(body,"id",cJSON_Duplicate(id,true));
    cJSON*result=cJSON_AddObjectToObject(body,"result");
    if(same(p,"method","initialize")) {
        cJSON_AddStringToObject(result,"protocolVersion","2024-11-05");
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(result,"capabilities"),"tools");
        cJSON*info=cJSON_AddObjectToObject(result,"serverInfo");
        cJSON_AddStringToObject(info,"name","weatherclock-c3");cJSON_AddStringToObject(info,"version","0.1");
    }else if(same(p,"method","tools/list")) {
        cJSON*tools=cJSON_AddArrayToObject(result,"tools");
        const char*names[]={"self.weatherclock.get_environment","self.weatherclock.show_clock"};
        const char*descriptions[]={"Read STM32 time, indoor temperature, humidity and validity bits","Return STM32 display to clock"};
        for(int i=0;i<2;i++) {
            cJSON*t=cJSON_CreateObject();cJSON_AddStringToObject(t,"name",names[i]);
            cJSON_AddStringToObject(t,"description",descriptions[i]);
            cJSON*s=cJSON_AddObjectToObject(t,"inputSchema");cJSON_AddStringToObject(s,"type","object");
            cJSON_AddObjectToObject(s,"properties");cJSON_AddItemToArray(tools,t);
        }
    }else if(same(p,"method","tools/call")) {
        const cJSON*params=cJSON_GetObjectItemCaseSensitive(p,"params");
        const char*command=nullptr;
        if(same(params,"name","self.weatherclock.get_environment"))command="@WC1 STATUS";
        if(same(params,"name","self.weatherclock.show_clock"))command="@WC1 HOME";
        char response[1024]={};
        bool ok=command&&!maintenance&&bridge_exchange(command,response,sizeof(response));
        cJSON_AddBoolToObject(result,"isError",!ok);
        cJSON*content=cJSON_AddArrayToObject(result,"content");cJSON*t=cJSON_CreateObject();
        cJSON_AddStringToObject(t,"type","text");cJSON_AddStringToObject(t,"text",ok?response:"STM32 unavailable or tool unsupported");
        cJSON_AddItemToArray(content,t);
    }else {
        cJSON_DeleteItemFromObject(body,"result");cJSON*error=cJSON_AddObjectToObject(body,"error");
        cJSON_AddNumberToObject(error,"code",-32601);cJSON_AddStringToObject(error,"message","Method not found");
    }
    send_json(reply);
}
static void lcd(const cJSON*o) {
    char command[256],response[1024];
    unsigned id=number(o,"id"),rev=number(o,"revision");
    bool valid=!maintenance&&rev==chat_revision;
    if(same(o,"op","start"))snprintf(command,sizeof(command),"@WC1 LCDSTART %u %u %u",id,rev,(unsigned)number(o,"sum"));
    else if(same(o,"op","data")&&strlen(str(o,"hex"))<=192)snprintf(command,sizeof(command),"@WC1 LCDDATA %u %u %s",id,(unsigned)number(o,"offset"),str(o,"hex"));
    else if(same(o,"op","end"))snprintf(command,sizeof(command),"@WC1 LCDEND %u",id);
    else valid=false;
    bool ok=valid&&bridge_exchange(command,response,sizeof(response));
    cJSON*ack=event("wc_lcd_ack");cJSON_AddStringToObject(ack,"ticket",str(o,"ticket"));cJSON_AddBoolToObject(ack,"ok",ok);send_json(ack);
}
static void process(const Message&m) {
    cJSON*o=cJSON_ParseWithLength(m.data,m.length);
    if(!o)return;
    if(same(o,"type","hello")) {
        const cJSON*a=cJSON_GetObjectItemCaseSensitive(o,"audio_params");
        const char*id=str(o,"session_id");
        if(same(o,"transport","websocket")&&same(a,"format","opus")&&number(a,"sample_rate")==16000&&number(a,"channels")==1&&number(a,"frame_duration")==60&&strlen(id)>0&&strlen(id)<sizeof(session_id)){
            strcpy(session_id,id);ready=true;ESP_LOGI(TAG,"Gateway hello accepted; heap=%lu",(unsigned long)esp_get_free_heap_size());
        }else ESP_LOGE(TAG,"Unsupported hello/audio format");
    }else if(ready&&(!*str(o,"session_id")||!strcmp(str(o,"session_id"),session_id))) {
        if(same(o,"type","mcp"))mcp(o);
        else if(same(o,"type","wc_lcd"))lcd(o);
        else if(same(o,"type","tts")) {
            if(same(o,"state","start")&&!maintenance) {
                const int revision=number(o,"wc_revision");
                if(phase==Phase::Idle&&revision>0) {
                    chat_revision=static_cast<unsigned>(revision);
                    audio_clear();
                }
                phase=Phase::Playing;
            }
            else if(same(o,"state","stop")){if(phase==Phase::Playing)audio_end();else if(phase==Phase::Waiting)phase=Phase::Idle;}
        }else if(same(o,"type","alert")){ESP_LOGW(TAG,"Gateway: %s",str(o,"message"));phase=Phase::Idle;audio_clear();}
    }
    cJSON_Delete(o);
}
static void ws_event(void*,esp_event_base_t,int32_t id,void*event_data) {
    static Message assembling={};
    static size_t used=0;
    if(id==WEBSOCKET_EVENT_CONNECTED) {
        ready=false;session_id[0]=0;
        send_text("{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\",\"features\":{\"mcp\":true},\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}");
    }else if(id==WEBSOCKET_EVENT_DISCONNECTED||id==WEBSOCKET_EVENT_ERROR) {
        ready=false;phase=Phase::Idle;audio_clear();used=0;
    }else if(id==WEBSOCKET_EVENT_DATA) {
        auto*d=static_cast<esp_websocket_event_data_t*>(event_data);
        if(d->op_code>=8)return;
        if(d->payload_offset==0&&(d->op_code==1||d->op_code==2)){assembling.opcode=d->op_code;used=0;}
        if(d->data_len<0||used+d->data_len>sizeof(assembling.data)){used=0;return;}
        memcpy(assembling.data+used,d->data_ptr,d->data_len);used+=d->data_len;
        if(d->fin&&d->payload_offset+d->data_len==d->payload_len) {
            assembling.length=used;
            if(assembling.opcode==2) {
                const auto state=phase.load();
                if(ready&&(state==Phase::Waiting||state==Phase::Playing))
                    audio_enqueue(reinterpret_cast<const uint8_t*>(assembling.data),used,audio_generation);
            }else if(xQueueSend(messages,&assembling,0)!=pdTRUE) {
                ESP_LOGW(TAG,"Control queue overrun");
            }
            used=0;
        }
    }
}
static void wifi_event(void*,esp_event_base_t base,int32_t id,void*) {
    if(base==WIFI_EVENT&&(id==WIFI_EVENT_STA_START||id==WIFI_EVENT_STA_DISCONNECTED))esp_wifi_connect();
    if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP)ESP_LOGI(TAG,"Wi-Fi connected");
}
static void network_init() {
    ESP_ERROR_CHECK(nvs_flash_init()); // Never erase existing Wi-Fi credentials on an error.
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi={};SavedNetwork saved={};size_t length=sizeof(saved);nvs_handle_t handle;
    if(nvs_open("wc-network",NVS_READONLY,&handle)==ESP_OK) {
        if(nvs_get_blob(handle,"config",&saved,&length)!=ESP_OK||length!=sizeof(saved)||!NetworkSettings::valid(saved))memset(&saved,0,sizeof(saved));
        nvs_close(handle);
    }
    if(!saved.ssid[0]){ESP_LOGE(TAG,"No saved Wi-Fi: configure it with the original C3 firmware first");abort();}
    memcpy(wifi.sta.ssid,saved.ssid,strlen(saved.ssid));memcpy(wifi.sta.password,saved.password,strlen(saved.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    memset(&saved,0,sizeof(saved));
}
static int listener(int port,int type) {
    int sock=socket(AF_INET,type,0);int yes=1;setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    sockaddr_in addr={};addr.sin_family=AF_INET;addr.sin_port=htons(port);addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(sock,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){close(sock);return -1;}
    if(type==SOCK_STREAM&&listen(sock,1)<0){close(sock);return -1;}return sock;
}
static void discovery(void*) {
    int sock=listener(8766,SOCK_DGRAM);configASSERT(sock>=0);
    for(;;) {
        char data[81],nonce[33],reply[240];sockaddr_in from={};socklen_t size=sizeof(from);
        int count=recvfrom(sock,data,sizeof(data),0,reinterpret_cast<sockaddr*>(&from),&size);
        if(count>0&&count<=80&&parseDiscoveryNonce(data,count,nonce)) {
            snprintf(reply,sizeof(reply),"{\"service\":\"weatherclock-voice\",\"version\":1,\"id\":\"wc-%02x%02x%02x%02x%02x%02x\",\"port\":8765,\"nonce\":\"%s\"}",
                     mac[5],mac[4],mac[3],mac[2],mac[1],mac[0],nonce);
            sendto(sock,reply,strlen(reply),0,reinterpret_cast<sockaddr*>(&from),size);
        }
    }
}
static bool write_all(int sock,const char*data) {
    size_t len=strlen(data),offset=0;
    while(offset<len){int sent=send(sock,data+offset,len-offset,0);if(sent<=0)return false;offset+=sent;}return true;
}
static void maintenance_task(void*) {
    int server=listener(8765,SOCK_STREAM);configASSERT(server>=0);
    for(;;) {
        int sock=accept(server,nullptr,nullptr);if(sock<0){vTaskDelay(pdMS_TO_TICKS(100));continue;}
        bool ota=false;size_t used=0;char line[256];bool discard=false;
        int keepalive=1;
        setsockopt(sock,SOL_SOCKET,SO_KEEPALIVE,&keepalive,sizeof(keepalive));
        for(;;) {
            char byte;int count=recv(sock,&byte,1,0);if(count<=0)break;
            if(byte=='\n') {
                line[used]=0;
                if(!discard) {
                    if(!strcmp(line,"PING"))write_all(sock,"VOICE_XIAOZHI1\n");
                    else if(!strcmp(line,"OTA_LINK"))write_all(sock,"OTA_LINK1\n");
                    else if(!strncmp(line,"WC @WC1 ",8)) {
                        char response[1024]={};const char*command=line+3;
                        if((!strncmp(command,"@WC1 OTADATA ",13)||!strcmp(command,"@WC1 OTAEND"))&&!ota) {
                            write_all(sock,"@WC1 ERR reason=OTA_NOT_STARTED\nWCEND\n");
                        }else {
                            const bool enter=!strcmp(command,"@WC1 OTAENTER");
                            const bool begin=!strncmp(command,"@WC1 OTABEGIN ",14);
                            const bool end=!strcmp(command,"@WC1 OTAEND");
                            if(enter||begin) maintenance=true;
                            bool ok=bridge_exchange(command,response,sizeof(response),begin||end?23000:1500);
                            if(begin) ota=ok&&strstr(response,"ACK cmd=OTABEGIN");
                            if(!ok||end) {ota=false;maintenance=false;}
                            write_all(sock,response);
                            if(!ok&&!strstr(response,"@WC1 ERR "))write_all(sock,"@WC1 ERR reason=PEER_TIMEOUT\n");
                            write_all(sock,"WCEND\n");
                        }
                    }else write_all(sock,"ERR maintenance-only firmware\n");
                }
                used=0;discard=false;
            }else if(byte!='\r'&&!discard){
                if(used+1<sizeof(line))line[used++]=byte;else discard=true;
            }
        }
        close(sock);maintenance=false;
    }
}
extern "C" void app_main() {
    ESP_LOGI(TAG,"WeatherClock Xiaozhi (IDF 5.5, protocol v1)");
    messages=xQueueCreate(6,sizeof(Message));configASSERT(messages);
    bridge_init();audio_init();network_init();esp_read_mac(mac,ESP_MAC_WIFI_STA);
    snprintf(device_id,sizeof(device_id),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(client_id,sizeof(client_id),"wc-%02x%02x%02x%02x%02x%02x-v1",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    snprintf(headers,sizeof(headers),"Authorization: Bearer %s\r\nProtocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",WC_DEVICE_TOKEN,device_id,client_id);
    esp_websocket_client_config_t config={};config.uri=WC_SERVER_URL;config.headers=headers;
    config.buffer_size=2048;config.task_stack=6144;config.reconnect_timeout_ms=5000;config.network_timeout_ms=5000;
    config.crt_bundle_attach=esp_crt_bundle_attach;
    config.enable_close_reconnect=true;
    websocket=esp_websocket_client_init(&config);configASSERT(websocket);
    ESP_ERROR_CHECK(esp_websocket_register_events(websocket,WEBSOCKET_EVENT_ANY,ws_event,nullptr));
    ESP_ERROR_CHECK(esp_websocket_client_start(websocket));
    // Mark the independently booted C3 image valid only after local peripherals and Wi-Fi start.
    if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK)ESP_LOGW(TAG,"No pending C3 rollback state");
    configASSERT(xTaskCreate(discovery,"wc-discovery",3072,nullptr,3,nullptr)==pdPASS);
    configASSERT(xTaskCreate(maintenance_task,"wc-maintenance",6144,nullptr,4,nullptr)==pdPASS);
    Message incoming;int64_t last_poll=0;
    for(;;) {
        if(xQueueReceive(messages,&incoming,pdMS_TO_TICKS(10))==pdTRUE)process(incoming);
        if(maintenance&&phase!=Phase::Idle)abort_turn();
        if(key_event.exchange(false)&&!maintenance)begin_turn();
        if(phase!=Phase::Idle&&!maintenance&&esp_timer_get_time()-last_poll>500000) {
            last_poll=esp_timer_get_time();if(!current_chat())abort_turn();
        }
    }
}
