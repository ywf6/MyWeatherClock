#include "probe.h"
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
static const char*TAG="wc-audio";
constexpr int32_t playback_volume_percent = 75;
static i2s_chan_handle_t tx,rx;
static QueueHandle_t playback;
static TaskHandle_t audio_task_handle;
static std::atomic<bool> stop_capture{false};
struct Frame {int32_t left,right;};
struct Packet {unsigned generation;uint16_t length;uint8_t bytes[WC_PACKET];};
static Frame raw[WC_FRAME];
static int16_t pcm[WC_FRAME];

static void capture() {
    static uint8_t encoded[4096];
    esp_opus_enc_config_t enc=ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate=WC_RATE;enc.channel=1;enc.bits_per_sample=16;enc.bitrate=24000;
    enc.frame_duration=ESP_OPUS_ENC_FRAME_DURATION_60_MS;enc.complexity=0;
    void*encoder=nullptr;
    auto setup=esp_opus_enc_open(&enc,sizeof(enc),&encoder);
    if(setup!=ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG,"Opus encoder setup failed: %d",setup);
        stop_recording();
        return;
    }
    int in_size=0,out_size=0;
    setup=esp_opus_enc_get_frame_size(encoder,&in_size,&out_size);
    if(setup!=ESP_AUDIO_ERR_OK||in_size!=static_cast<int>(sizeof(pcm))||out_size<=0||out_size>static_cast<int>(sizeof(encoded))) {
        ESP_LOGW(TAG,"Invalid Opus frame size: %d/%d",in_size,out_size);
        esp_opus_enc_close(encoder);
        stop_recording();
        return;
    }
    size_t bytes=0;
    while(i2s_channel_read(rx,raw,sizeof(raw),&bytes,0)==ESP_OK&&bytes){}
    int64_t until=esp_timer_get_time()+6000000;
    unsigned packets=0;
    while(phase==Phase::Recording&&!maintenance&&!stop_capture&&esp_timer_get_time()<until) {
        bytes=0;
        if(i2s_channel_read(rx,raw,sizeof(raw),&bytes,200)!=ESP_OK||bytes!=sizeof(raw)) {
            ESP_LOGW(TAG,"I2S capture failed");
            break;
        }
        for(int i=0;i<WC_FRAME;i++)pcm[i]=static_cast<int16_t>(raw[i].left>>16);
        esp_audio_enc_in_frame_t in={};in.buffer=reinterpret_cast<uint8_t*>(pcm);in.len=sizeof(pcm);
        esp_audio_enc_out_frame_t out={};out.buffer=encoded;out.len=sizeof(encoded);
        auto status=esp_opus_enc_process(encoder,&in,&out);
        if(status!=ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG,"Opus encode failed: %d",status);
            break;
        }
        if(phase!=Phase::Recording)break;
        if(esp_websocket_client_send_bin(websocket,reinterpret_cast<char*>(encoded),out.encoded_bytes,pdMS_TO_TICKS(500))<0) {
            ESP_LOGW(TAG,"Audio upload failed");
            break;
        }
        ++packets;
    }
    esp_opus_enc_close(encoder);
    ESP_LOGI(TAG,"Uploaded %u Opus packets",packets);
    if(phase==Phase::Recording)stop_recording();
}

static void play(void*decoder,Packet &packet) {
    esp_audio_dec_in_raw_t in={};in.buffer=packet.bytes;in.len=packet.length;
    esp_audio_dec_out_frame_t out={};out.buffer=reinterpret_cast<uint8_t*>(pcm);out.len=sizeof(pcm);
    esp_audio_dec_info_t info={};
    auto status=esp_opus_dec_decode(decoder,&in,&out,&info);
    if(status!=ESP_AUDIO_ERR_OK||info.sample_rate!=WC_RATE||info.channel!=1) {
        ESP_LOGW(TAG,"Opus decode failed: %d",status);
        return;
    }
    size_t count=out.decoded_size/2;
    if(count>WC_FRAME)return;
    // Apply speaker gain while converting PCM16 to 32-bit I2S slots.
    constexpr int32_t pcm_to_i2s = 65536 * playback_volume_percent / 100;
    for(size_t i=0;i<count;i++) {
        int32_t sample=static_cast<int32_t>(pcm[i])*pcm_to_i2s;
        raw[i]={sample,sample};
    }
    if(packet.generation!=audio_generation.load())return;
    size_t written=0;
    if(i2s_channel_write(tx,raw,count*sizeof(Frame),&written,200)!=ESP_OK)ESP_LOGW(TAG,"I2S output timeout");
}

static void audio_task(void*) {
    Packet packet;
    void*decoder=nullptr;
    unsigned generation=~0u;
    unsigned played=0;
    for(;;) {
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if(decoder&&generation!=audio_generation.load()) {
            esp_opus_dec_close(decoder);
            decoder=nullptr;
        }
        if(phase==Phase::Recording&&!stop_capture)capture();
        while(xQueueReceive(playback,&packet,0)==pdTRUE) {
            if(packet.generation!=audio_generation.load()||maintenance)continue;
            if(packet.length==0) {
                if(decoder)esp_opus_dec_close(decoder);
                decoder=nullptr;
                if(phase==Phase::Playing)phase=Phase::Idle;
                ESP_LOGI(TAG,"Processed %u playback packets",played);
                continue;
            }
            if(packet.generation!=generation) {
                if(decoder)esp_opus_dec_close(decoder);
                decoder=nullptr;
                generation=packet.generation;
                played=0;
                esp_opus_dec_cfg_t config={};config.sample_rate=WC_RATE;config.channel=1;
                config.frame_duration=ESP_OPUS_DEC_FRAME_DURATION_60_MS;config.self_delimited=false;
                auto status=esp_opus_dec_open(&config,sizeof(config),&decoder);
                if(status!=ESP_AUDIO_ERR_OK) {
                    decoder=nullptr;
                    ESP_LOGW(TAG,"Opus decoder setup failed: %d",status);
                }
            }
            if(decoder) {
                play(decoder,packet);
                ++played;
            }
        }
    }
}

void audio_capture_start(){stop_capture=false;xTaskNotifyGive(audio_task_handle);}
void audio_request_stop(){stop_capture=true;}
void audio_enqueue(const uint8_t*data,size_t length,unsigned generation) {
    if(!length||length>WC_PACKET||maintenance)return;
    Packet packet={};packet.generation=generation;packet.length=length;memcpy(packet.bytes,data,length);
    if(xQueueSend(playback,&packet,0)!=pdTRUE){ESP_LOGW(TAG,"Playback overrun; cancelling");audio_clear();phase=Phase::Idle;}
    else xTaskNotifyGive(audio_task_handle);
}
void audio_end() {
    Packet packet={};packet.generation=audio_generation;
    if(xQueueSend(playback,&packet,pdMS_TO_TICKS(100))==pdTRUE)xTaskNotifyGive(audio_task_handle);
}
void audio_clear() {
    ++audio_generation;stop_capture=true;
    if(playback)xQueueReset(playback);
    if(audio_task_handle)xTaskNotifyGive(audio_task_handle);
}
void audio_init() {
    i2s_chan_config_t channels=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    channels.dma_desc_num=6;channels.dma_frame_num=240;
    ESP_ERROR_CHECK(i2s_new_channel(&channels,&tx,&rx));
    i2s_std_config_t config={};
    config.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(WC_RATE);
    config.slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_STEREO);
    config.gpio_cfg.mclk=I2S_GPIO_UNUSED;config.gpio_cfg.bclk=GPIO_NUM_4;
    config.gpio_cfg.ws=GPIO_NUM_5;config.gpio_cfg.dout=GPIO_NUM_6;config.gpio_cfg.din=GPIO_NUM_7;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx,&config));ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx,&config));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));ESP_ERROR_CHECK(i2s_channel_enable(rx));
    playback=xQueueCreate(10,sizeof(Packet));configASSERT(playback);
    // Recording and playback alternate, so they can share the Opus scratch stack.
    configASSERT(xTaskCreate(audio_task,"wc-audio",28 * 1024,nullptr,6,&audio_task_handle)==pdPASS);
}
