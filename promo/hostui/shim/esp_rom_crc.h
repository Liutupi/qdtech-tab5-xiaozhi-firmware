#pragma once
#include <stdint.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* buf, uint32_t len) {
  crc = ~crc; for (uint32_t i=0;i<len;i++){ crc ^= buf[i]; for(int k=0;k<8;k++) crc = (crc>>1) ^ (0xEDB88320u & (-(int32_t)(crc&1))); } return ~crc; }
