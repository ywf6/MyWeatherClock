#pragma once
#include <string.h>

inline bool parseDiscoveryNonce(const char *packet, size_t length, char nonce[33]) {
  if ((length != 45 && !(length == 46 && packet[45] == '\n')) ||
      strncmp(packet, "WC_DISCOVER1 ", 13) != 0) return false;
  for (int i = 0; i < 32; ++i) {
    const char c = packet[13 + i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    nonce[i] = c;
  }
  nonce[32] = '\0'; return true;
}
