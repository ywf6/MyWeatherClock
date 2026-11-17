#pragma once
#include <stddef.h>
#include <stdint.h>
#include <atomic>
#include "esp_websocket_client.h"
constexpr int WC_RATE=16000, WC_FRAME=960, WC_PACKET=1275;
enum class Phase {Idle,Recording,Waiting,Playing};
extern std::atomic<Phase> phase;
extern std::atomic<bool> key_event,maintenance;
extern std::atomic<unsigned> audio_generation;
extern esp_websocket_client_handle_t websocket;
extern char session_id[65];
extern unsigned chat_revision;
bool bridge_exchange(const char*,char*,size_t,int timeout_ms=1500);
void bridge_init();
void audio_init();
void audio_capture_start();
void audio_request_stop();
void audio_enqueue(const uint8_t*,size_t,unsigned);
void audio_end();
void audio_clear();
bool send_text(const char*);
void stop_recording();
