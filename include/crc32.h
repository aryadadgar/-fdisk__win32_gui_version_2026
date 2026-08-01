// crc32.h - standard CRC-32 (IEEE 802.3, poly 0xEDB88320), used by the UEFI
// GPT spec for both the header CRC and the partition-entry-array CRC.
#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

class Crc32 {
public:
    static uint32_t Compute(const uint8_t* data, size_t len) {
        const auto& table = Table();
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) {
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

private:
    static const std::array<uint32_t, 256>& Table() {
        static const std::array<uint32_t, 256> table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            return t;
        }();
        return table;
    }
};
