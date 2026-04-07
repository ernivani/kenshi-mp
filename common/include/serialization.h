#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "packets.h"

namespace kmp {

// ---------------------------------------------------------------------------
// Serialization helpers for raw packet structs over ENet
// ---------------------------------------------------------------------------

/// Pack any packet struct into a byte buffer suitable for enet_packet_create.
template <typename T>
inline std::vector<uint8_t> pack(const T& packet) {
    std::vector<uint8_t> buf(sizeof(T));
    std::memcpy(buf.data(), &packet, sizeof(T));
    return buf;
}

/// Unpack a byte buffer into a packet struct.
/// Returns true and fills 'out' if successful, false if buffer too small.
template <typename T>
inline bool unpack(const uint8_t* data, size_t length, T& out) {
    if (length < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data, sizeof(T));
    return true;
}

/// Read just the header from raw data to determine packet type.
/// Returns true and fills 'out' if successful.
inline bool peek_header(const uint8_t* data, size_t length, PacketHeader& out) {
    return unpack<PacketHeader>(data, length, out);
}

/// Validate that a packet header has the expected protocol version.
inline bool validate_version(const PacketHeader& header) {
    return header.version == PROTOCOL_VERSION;
}

// ---------------------------------------------------------------------------
// Safe string copy into fixed-size char arrays (null-terminated)
// ---------------------------------------------------------------------------
template <size_t N>
inline void safe_strcpy(char (&dst)[N], const char* src) {
    std::strncpy(dst, src, N - 1);
    dst[N - 1] = '\0';
}

} // namespace kmp
