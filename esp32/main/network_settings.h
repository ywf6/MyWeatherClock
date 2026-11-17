/* [阅读说明]
 * C3 网络配置状态：saved 为已保存配置，pending 为待验证配置。
 * 新配置先尝试连接，voice_firmware.cpp 确认成功后才调用 commit() 持久化，失败可回退旧配置。
 */
#pragma once
#include <stdint.h>
#include <string.h>

struct SavedNetwork {
  uint32_t version;
  char ssid[33];
  char password[64];
};

class NetworkSettings {
 public:
  SavedNetwork saved{}, pending{};
  bool hasSaved = false, hasPending = false, saveFailed = false;
  static bool valid(const SavedNetwork &config) {
    if (config.version != 0x57434e31 || !memchr(config.ssid, 0, sizeof(config.ssid)) ||
        !memchr(config.password, 0, sizeof(config.password))) return false;
    const size_t ssid = strlen(config.ssid), password = strlen(config.password);
    return ssid > 0 && ssid <= 32 && (password == 0 || (password >= 8 && password <= 63));
  }
  /* [阅读说明]
   * 一次读取完整配置结构，检查版本、字符串终止符及长度，避免使用不完整配置。
   */
  template <class Storage> bool load(Storage &storage) {
    SavedNetwork config{};
    hasSaved = storage.getBytesLength("config") == sizeof(config) &&
      storage.getBytes("config", &config, sizeof(config)) == sizeof(config) && valid(config);
    saved = hasSaved ? config : SavedNetwork{};
    return hasSaved;
  }
  /* [阅读说明]
   * 把新 SSID/密码放入 RAM；此时还未覆盖 NVS 中的已保存配置。
   */
  bool stage(const char *ssid, const char *password) {
    SavedNetwork config{};
    if (strlen(ssid) > 32 || strlen(password) > 63) return false;
    config.version = 0x57434e31;
    memcpy(config.ssid, ssid, strlen(ssid));
    memcpy(config.password, password, strlen(password));
    if (!valid(config)) return false;
    pending = config; hasPending = true; saveFailed = false; return true;
  }
  /* [阅读说明]
   * 连接成功后将整个配置写入单个 NVS 键，相同配置不重复写；完成后清空待定密码副本。
   */
  template <class Storage> bool commit(Storage &storage) {
    if (!hasPending) return false;
    const bool unchanged = hasSaved && memcmp(&saved, &pending, sizeof(saved)) == 0;
    const bool ok = unchanged || storage.putBytes("config", &pending, sizeof(pending)) == sizeof(pending);
    if (ok) { saved = pending; hasSaved = true; }
    saveFailed = !ok; hasPending = false;
    memset(&pending, 0, sizeof(pending));
    return ok;
  }
};
