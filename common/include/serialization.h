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

/// Pack a fixed-size struct followed by a variable-length byte tail.
/// Used for SnapshotUploadChunk and any future variable-length packets.
template <typename T>
inline std::vector<uint8_t> pack_with_tail(const T& packet, const uint8_t* tail, size_t tail_len) {
    std::vector<uint8_t> buf(sizeof(T) + tail_len);
    std::memcpy(buf.data(), &packet, sizeof(T));
    if (tail_len > 0 && tail) {
        std::memcpy(buf.data() + sizeof(T), tail, tail_len);
    }
    return buf;
}

/// Inverse of pack_with_tail: writes the fixed prefix into `out`, and sets
/// `tail_ptr` and `tail_len` to point to the trailing bytes inside `data`.
/// The tail is NOT copied — `data` must outlive the caller's use of `tail_ptr`.
/// Returns true on success, false if `length` is too small.
template <typename T>
inline bool unpack_with_tail(const uint8_t* data, size_t length, T& out,
                             const uint8_t*& tail_ptr, size_t& tail_len) {
    if (length < sizeof(T)) return false;
    std::memcpy(&out, data, sizeof(T));
    tail_ptr = data + sizeof(T);
    tail_len = length - sizeof(T);
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
