#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <optional>
#include "packets.h"

namespace kmp {

// ---------------------------------------------------------------------------
// Serialization helpers for raw packet structs over ENet
//
// Since we use #pragma pack(1) POD structs, serialization is just memcpy.
// These helpers add type-safety and bounds checking.
// ---------------------------------------------------------------------------

/// Pack any packet struct into a byte buffer suitable for enet_packet_create.
template <typename T>
inline std::vector<uint8_t> pack(const T& packet) {
    static_assert(std::is_trivially_copyable_v<T>, "Packet must be trivially copyable");
    std::vector<uint8_t> buf(sizeof(T));
    std::memcpy(buf.data(), &packet, sizeof(T));
    return buf;
}

/// Unpack a byte buffer into a packet struct.
/// Returns std::nullopt if the buffer is too small.
template <typename T>
inline std::optional<T> unpack(const uint8_t* data, size_t length) {
    static_assert(std::is_trivially_copyable_v<T>, "Packet must be trivially copyable");
    if (length < sizeof(T)) {
        return std::nullopt;
    }
    T packet;
    std::memcpy(&packet, data, sizeof(T));
    return packet;
}

/// Read just the header from raw data to determine packet type.
/// Returns std::nullopt if the buffer is too small for a header.
inline std::optional<PacketHeader> peek_header(const uint8_t* data, size_t length) {
    return unpack<PacketHeader>(data, length);
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
