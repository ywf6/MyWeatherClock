/* [阅读说明]
 * 外置 ESP32-C3 固件：I2S 采音/播音、USB/TCP 传输、UART 转发 STM32 命令。
 * 电脑侧 USB 460800；STM32 侧 Serial1=115200，GPIO0=TX、GPIO1=RX。
 * 局域网 TCP 8765 承载会话，UDP 8766 用于设备发现；完整 AI 调用和离线唤醒检测在电脑上。
 */
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include "network_settings.h"
#include "discovery_protocol.h"

WiFiServer voiceServer(8765);
WiFiClient voiceClient;
bool useNetwork = false;
bool serverStarted = false;
Preferences networkStorage;
NetworkSettings networkSettings;
WiFiUDP discoverySocket;
bool storageReady = false;
uint32_t pendingSince = 0, lastReconnect = 0;
void networkTick();
/* [阅读说明]
 * 统一 USB 与 TCP 的读写接口；当前只选择一个主机通道，TCP 会话连接后拥有该通道。
 */
Stream &host() {
  return useNetwork ? static_cast<Stream &>(voiceClient) : static_cast<Stream &>(Serial);
}

#include "wake_reply_pcm.h"

/* [阅读说明]
 * 主机传输格式为 16 kHz、单声道、PCM16；I2S 使用 32 位双声道时隙，在下面做格式转换。
 */
constexpr int kSampleRate = 16000;
constexpr size_t kBlockFrames = 240;
constexpr size_t kPacketSamples = 8 * kBlockFrames;
constexpr size_t kRecordSamples = 5 * kSampleRate;
constexpr size_t kMaxPlaybackSamples = 30 * kSampleRate;

struct StereoFrame {
  int32_t left;
  int32_t right;
};

I2SClass audio;
StereoFrame i2sBlock[kBlockFrames];
int16_t pcmPacket[kPacketSamples];
bool audioReady;

bool readMicBlock() {
  return audio.readBytes(reinterpret_cast<char *>(i2sBlock), sizeof(i2sBlock)) == sizeof(i2sBlock);
}

/* [阅读说明]
 * 从左声道的 32 位 I2S 数据取高 16 位，生成主机使用的 PCM16。
 */
bool captureMic(size_t frames) {
  const bool captured = readMicBlock();
  for (size_t i = 0; i < frames; ++i) {
    pcmPacket[i] = captured ? static_cast<int16_t>(i2sBlock[i].left >> 16) : 0;
  }
  return captured;
}

bool readKey2Event() {
  static char line[8];
  static size_t used = 0;
  static bool discard = false;
  bool pressed = false;
  for (unsigned budget = 0; budget < 32 && Serial1.available(); ++budget) {
    const int byte = Serial1.read();
    if (byte == '\n') {
      line[used] = '\0';
      pressed |= !discard && strcmp(line, "WCKEY2") == 0;
      used = 0;
      discard = false;
    } else if (byte != '\r' && !discard) {
      if (used < sizeof(line) - 1) {
        line[used++] = static_cast<char>(byte);
      } else {
        discard = true;
      }
    }
  }
  return pressed;
}

/* [阅读说明]
 * 连续采音协议：标记 1 后跟 480 字节音频，2 表示 K2，0 表示结束；电脑用 '.' 保活、其他字节停止。
 * 此处只提供音频流，唤醒词识别由电脑的 Python 进程执行。
 */
void streamMic(bool withKey2) {
  host().println("STREAM");
  uint32_t lastKeepalive = millis();
  while (millis() - lastKeepalive < 2000) {
    networkTick();
    if (host().available()) {
      if (host().read() != '.') {
        break;
      }
      lastKeepalive = millis();
    }
    if (readKey2Event() && withKey2) {
      host().write(static_cast<uint8_t>(2));
    }
    if (!captureMic(kBlockFrames)) {
      host().write(static_cast<uint8_t>(0xff));
      return;
    }
    host().write(static_cast<uint8_t>(1));
    host().write(reinterpret_cast<const uint8_t *>(pcmPacket), kBlockFrames * sizeof(int16_t));
  }
  host().write(static_cast<uint8_t>(0));
}

/* [阅读说明]
 * 固定录音约 5 秒：PCM 160000 头 -> 二进制音频 -> DONE/错误；先丢弃少量陈旧 I2S 输入。
 */
void recordAudio() {
  // 丢弃空闲期间积压的输入，从接近当前时刻的位置开始录音。
  for (int i = 0; i < 6; ++i) {
    if (!readMicBlock()) {
      host().println("ERR capture");
      return;
    }
  }
  host().printf("PCM %u\n", static_cast<unsigned>(kRecordSamples * sizeof(int16_t)));
  bool captured = true;
  for (size_t offset = 0; offset < kRecordSamples; offset += kBlockFrames) {
    networkTick();
    const size_t frames = min(kBlockFrames, kRecordSamples - offset);
    captured &= captureMic(frames);
    host().write(reinterpret_cast<const uint8_t *>(pcmPacket), frames * sizeof(int16_t));
  }
  host().println(captured ? "DONE" : "ERR capture");
}

bool outputPacket(size_t count) {
  networkTick();
  bool played = true;
  for (size_t start = 0; start < count; start += kBlockFrames) {
    const size_t frames = min(kBlockFrames, count - start);
    for (size_t i = 0; i < frames; ++i) {
      const int32_t word = static_cast<int32_t>(pcmPacket[start + i]) * 65536;
      i2sBlock[i] = {word, word};
    }
    const size_t blockBytes = frames * sizeof(StereoFrame);
    played &= audio.write(reinterpret_cast<const uint8_t *>(i2sBlock), blockBytes) == blockBytes;
  }
  return played;
}

bool playWakeReply() {
  bool played = true;
  constexpr size_t samples = sizeof(kWakeReplyPcm) / sizeof(kWakeReplyPcm[0]);
  for (size_t offset = 0; offset < samples; offset += kPacketSamples) {
    const size_t count = min(kPacketSamples, samples - offset);
    memcpy(pcmPacket, kWakeReplyPcm + offset, count * sizeof(int16_t));
    played &= outputPacket(count);
  }
  delay(100);
  return played;
}

/* [阅读说明]
 * 每包最多 1920 个样本=3840 字节，完整接收后回 OK，再由调用者播放，限制主机发送速度。
 */
bool receivePacket(size_t count) {
  const size_t bytes = count * sizeof(int16_t);
  if (host().readBytes(reinterpret_cast<char *>(pcmPacket), bytes) != bytes) {
    host().println("ERR playback timeout");
    return false;
  }
  // 回执后主机可发送下一包，当前包同时送入 I²S。
  host().println("OK");
  return true;
}

void finishPlayback(bool played) {
  delay(100);
  host().println(played ? "DONE" : "ERR playback");
}

void playAudio(size_t samples) {
  if (samples == 0 || samples > kMaxPlaybackSamples) {
    host().println("ERR playback length");
    return;
  }
  host().println("READY");
  bool played = true;
  for (size_t offset = 0; offset < samples; offset += kPacketSamples) {
    const size_t count = min(kPacketSamples, samples - offset);
    if (!receivePacket(count)) {
      return;
    }
    played &= outputPacket(count);
  }
  finishPlayback(played);
}

void playMusic() {
  host().println("READY");
  bool played = true;
  while (true) {
    uint8_t header[2];
    if (host().readBytes(header, sizeof(header)) != sizeof(header)) {
      host().println("ERR music timeout");
      return;
    }
    const size_t count = header[0] | (static_cast<size_t>(header[1]) << 8);
    if (count == 0) {
      finishPlayback(played);
      return;
    }
    if (count > kPacketSamples) {
      host().println("ERR music packet");
      return;
    }
    if (!receivePacket(count)) {
      return;
    }
    played &= outputPacket(count);
  }
}

static bool otaTransfer = false;

/* [阅读说明]
 * 将 WC 命令转发至 STM32，读取到 WCEND 才结束本次应答；普通命令和 OTA 校验使用不同超时。
 */
void forwardWeatherClock(const char *command) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(command);
  const uint32_t started = millis();
  char reply[256];
  size_t length = 0;
  // Initial ownership scan and full-image verification can take longer than normal UI commands.
  const uint32_t timeout = (!strncmp(command, "@WC1 OTABEGIN ", 14) ||
                            !strcmp(command, "@WC1 OTAEND")) ? 20000 : 1200;
  while (millis() - started < timeout && (!useNetwork || voiceClient.connected())) {
    const int byte = Serial1.read();
    if (byte < 0) {
      delay(1);
    } else if (byte == '\n') {
      reply[length] = '\0';
      // A press while processing a request is not a new listening session.
      if (!strcmp(reply, "@WC1 ACK cmd=OTABEGIN")) otaTransfer = true;
      if (strcmp(reply, "WCKEY2") != 0) host().println(reply);
      if (strcmp(reply, "WCEND") == 0) return;
      length = 0;
    } else if (byte != '\r') {
      if (length == sizeof(reply) - 1) break;
      reply[length++] = static_cast<char>(byte);
    }
  }
  host().println("@WC1 ERR reason=PEER_TIMEOUT");
  host().println("WCEND");
}

/* [阅读说明]
 * 初始化音频、串口和 NVS；若存在已保存网络，则尝试自动连接。
 */
void setup() {
  Serial.setRxBufferSize(8192);
  Serial.setTxBufferSize(2048);
  Serial.begin(460800);
  Serial.setTimeout(1500);
  Serial1.begin(115200, SERIAL_8N1, 1, 0);
  // GPIO4 BCLK、GPIO5 WS、GPIO6 功放 DIN、GPIO7 麦克风 SD。
  audio.setPins(4, 5, 6, 7);
  audioReady = audio.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  // 仅由本应用在首次连接成功时原子保存一条 NVS 配置。
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  storageReady = networkStorage.begin("wc-network", false);
  if (storageReady && networkSettings.load(networkStorage)) {
    WiFi.begin(networkSettings.saved.ssid, networkSettings.saved.password);
    lastReconnect = millis();
  }
  delay(350);
}

bool decodeHex(const char *hex, char *output, size_t capacity) {
  const size_t length = strlen(hex);
  if (length % 2 || length / 2 >= capacity) return false;
  for (size_t i = 0; i < length; i += 2) {
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int high = digit(hex[i]), low = digit(hex[i + 1]);
    if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
    output[i / 2] = static_cast<char>((high << 4) | low);
  }
  output[length / 2] = '\0';
  return true;
}

/* [阅读说明]
 * 只允许通过 USB 串口配置热点；先 stage 到 RAM，实际连通后再提交 NVS。
 */
void joinHotspot(const char *arguments) {
  char ssidHex[65], passwordHex[129], extra;
  char ssid[33] = {}, password[64] = {};
  if (useNetwork || sscanf(arguments, "%64s %128s %c", ssidHex, passwordHex, &extra) != 2 ||
      !decodeHex(ssidHex, ssid, sizeof(ssid)) || !strlen(ssid) ||
      (strcmp(passwordHex, "-") != 0 && !decodeHex(passwordHex, password, sizeof(password))) ||
      (strlen(password) != 0 && strlen(password) < 8)) {
    host().println("ERR network configuration");
  } else {
    networkSettings.stage(ssid, password);
    pendingSince = millis();
    WiFi.disconnect(false, false);
    WiFi.begin(ssid, password);
    host().println("NETJOIN OK");
  }
  memset(password, 0, sizeof(password));
  memset(passwordHex, 0, sizeof(passwordHex));
}

/* [阅读说明]
 * 推进联网/重连、确认并保存待定配置，同时回复带本次 nonce 的 UDP 发现请求。
 */
void networkTick() {
  const uint32_t now = millis();
  if (networkSettings.hasPending) {
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == networkSettings.pending.ssid && now - pendingSince >= 500) {
      if (storageReady) networkSettings.commit(networkStorage);
      else { networkSettings.saveFailed = true; networkSettings.hasPending = false; memset(&networkSettings.pending, 0, sizeof(networkSettings.pending)); }
    } else if (now - pendingSince > 30000) {
      networkSettings.hasPending = false;
      memset(&networkSettings.pending, 0, sizeof(networkSettings.pending));
      if (networkSettings.hasSaved) WiFi.begin(networkSettings.saved.ssid, networkSettings.saved.password);
      lastReconnect = now;
    }
  } else if (WiFi.status() != WL_CONNECTED && networkSettings.hasSaved && now - lastReconnect >= 15000) {
    WiFi.reconnect();
    lastReconnect = now;
  }
  if (!serverStarted && WiFi.status() == WL_CONNECTED) {
    voiceServer.begin();
    voiceServer.setNoDelay(true);
    discoverySocket.begin(8766);
    serverStarted = true;
  }
  if (!serverStarted || WiFi.status() != WL_CONNECTED) return;
  const int size = discoverySocket.parsePacket();
  if (size <= 0) return;
  char packet[81];
  const int length = discoverySocket.read(packet, sizeof(packet) - 1);
  if (size > 80 || length < 45) return;
  packet[length] = '\0';
  char nonce[33];
  if (!parseDiscoveryNonce(packet, length, nonce)) return;
  char reply[224];
  snprintf(reply, sizeof(reply), "{\"service\":\"weatherclock-voice\",\"version\":1,\"id\":\"wc-%012llx\",\"port\":8765,\"nonce\":\"%s\"}",
           static_cast<unsigned long long>(ESP.getEfuseMac()), nonce);
  discoverySocket.beginPacket(discoverySocket.remoteIP(), discoverySocket.remotePort());
  discoverySocket.write(reinterpret_cast<const uint8_t *>(reply), strlen(reply));
  discoverySocket.endPacket();
}

/* [阅读说明]
 * 主循环按文本命令切换转发、配网、采音和播音模式；音频负载按长度读取，不能当换行文本解析。
 */
void loop() {
  networkTick();
  // One network session owns the channel until disconnected.
  if (useNetwork && !voiceClient.connected()) {
    voiceClient.stop();
    useNetwork = false;
    otaTransfer = false;
  }
  if (!useNetwork && serverStarted) {
    WiFiClient incoming = voiceServer.accept();
    if (incoming) {
      voiceClient = incoming;
      voiceClient.setTimeout(1500);
      voiceClient.setNoDelay(true);
      useNetwork = true;
    }
  }
  if (useNetwork) {
    while (Serial.available()) Serial.read();
  }
  if (!host().available()) {
    const bool key2 = readKey2Event();
    // 空闲时丢弃麦克风数据，避免下一次录音读到陈旧数据。
    if (audioReady) {
      if (key2) {
        playWakeReply();
      } else {
        readMicBlock();
      }
    } else {
      delay(20);
    }
    return;
  }

  char command[256];
  const size_t length = host().readBytesUntil('\n', command, sizeof(command) - 1);
  command[length] = '\0';
  if (strcmp(command, "OTA_LINK") == 0) {
    host().println(useNetwork ? "OTA_LINK1" : "ERR WIFI_REQUIRED");
  } else if (strncmp(command, "WC @WC1 ", 8) == 0) {
    if (!useNetwork && strncmp(command + 8, "OTA", 3) == 0) {
      host().println("@WC1 ERR reason=WIFI_REQUIRED");
      host().println("WCEND");
    } else if (!otaTransfer && (!strncmp(command + 8, "OTADATA ", 8) ||
                                !strcmp(command + 8, "OTAEND"))) {
      host().println("@WC1 ERR reason=OTA_NOT_STARTED");
      host().println("WCEND");
    } else {
      if (!strncmp(command + 8, "OTABEGIN ", 9) || !strcmp(command + 8, "OTAEND"))
        otaTransfer = false;
      forwardWeatherClock(command + 3);
    }
  } else if (strncmp(command, "NETJOIN ", 8) == 0) {
    joinHotspot(command + 8);
    memset(command, 0, sizeof(command));
  } else if (strcmp(command, "NETINFO") == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      host().print("NET CONNECTED ");
      host().println(WiFi.localIP());
    } else {
      host().println("NET WAITING");
    }
  } else if (strcmp(command, "NETCFG") == 0) {
    host().println(networkSettings.hasPending ? "NETCFG PENDING" :
      networkSettings.saveFailed ? "NETCFG ERROR" : networkSettings.hasSaved ? "NETCFG SAVED" : "NETCFG EMPTY");
  } else if (!audioReady) {
    host().println("ERR I2S initialization");
  } else if (strcmp(command, "PING") == 0) {
    host().println("VOICE1");
  } else if (strcmp(command, "WAKE_REPLY") == 0) {
    host().println(playWakeReply() ? "DONE" : "ERR wake reply");
  } else if (strcmp(command, "REC") == 0) {
    recordAudio();
  } else if (strcmp(command, "LISTEN") == 0) {
    streamMic(false);
  } else if (strcmp(command, "WC_LISTEN") == 0) {
    streamMic(true);
  } else if (strcmp(command, "MUSIC") == 0) {
    playMusic();
  } else if (strncmp(command, "PLAY ", 5) == 0) {
    char *end;
    const unsigned long samples = strtoul(command + 5, &end, 10);
    if (*end == '\0') {
      playAudio(samples);
    } else {
      host().println("ERR playback length");
    }
  } else {
    host().println("ERR command");
  }
}
