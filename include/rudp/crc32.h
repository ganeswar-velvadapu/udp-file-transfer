#pragma once

#include <cstddef>
#include <cstdint>

namespace rudp {

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320).
// Chainable: crc32_update(crc32_update(0, a), b) == crc32(a + b).
uint32_t crc32_update(uint32_t crc, const void* data, size_t len);

inline uint32_t crc32(const void* data, size_t len) {
    return crc32_update(0, data, len);
}

}  // namespace rudp
