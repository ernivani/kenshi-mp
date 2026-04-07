#pragma once

#include <cstdint>
#include <cstring>
#include "protocol.h"

namespace kmp {

// ---------------------------------------------------------------------------
// Packet types
// ---------------------------------------------------------------------------
enum class PacketType : uint8_t {
    CONNECT_REQUEST  = 0x01,   // client -> server: join with player name
    CONNECT_ACCEPT   = 0x02,   // server -> client: assigned player ID
    CONNECT_REJECT   = 0x03,   // server -> client: reason string
    PLAYER_STATE     = 0x10,   // bidirectional: position, rotation, animation
    PLAYER_DISCONNECT= 0x11,   // server -> client: remove player NPC
    SPAWN_NPC        = 0x20,   // server -> client: create NPC for remote player
    CHAT_MESSAGE     = 0x30,   // bidirectional
    PING             = 0xF0,
    PONG             = 0xF1,
};

// ---------------------------------------------------------------------------
// Packet header — prefixed to every packet
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

struct PacketHeader {
    uint8_t    version;    // PROTOCOL_VERSION
    PacketType type;
};

// ---------------------------------------------------------------------------
// Packet payloads
// ---------------------------------------------------------------------------

struct ConnectRequest {
    PacketHeader header;
    char         name[MAX_NAME_LENGTH];
    char         model[MAX_MODEL_LENGTH];

    ConnectRequest() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CONNECT_REQUEST;
    }
};

struct ConnectAccept {
    PacketHeader header;
    uint32_t     player_id;

    ConnectAccept() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CONNECT_ACCEPT;
    }
};

struct ConnectReject {
    PacketHeader header;
    char         reason[128];

    ConnectReject() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CONNECT_REJECT;
    }
};

struct PlayerState {
    PacketHeader header;
    uint32_t     player_id;
    float        x, y, z;          // world position
    float        yaw;              // rotation around Y axis (radians)
    uint32_t     animation_id;     // current animation state hash
    float        speed;            // movement speed (for animation blending)

    PlayerState() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::PLAYER_STATE;
    }
};

struct PlayerDisconnect {
    PacketHeader header;
    uint32_t     player_id;

    PlayerDisconnect() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::PLAYER_DISCONNECT;
    }
};

struct SpawnNPC {
    PacketHeader header;
    uint32_t     player_id;
    char         name[MAX_NAME_LENGTH];
    char         model[MAX_MODEL_LENGTH];
    float        x, y, z;
    float        yaw;

    SpawnNPC() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SPAWN_NPC;
    }
};

struct ChatMessage {
    PacketHeader header;
    uint32_t     player_id;        // 0 = server message
    char         message[MAX_CHAT_LENGTH];

    ChatMessage() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CHAT_MESSAGE;
    }
};

struct PingPacket {
    PacketHeader header;
    uint64_t     timestamp_ms;

    PingPacket() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::PING;
    }
};

struct PongPacket {
    PacketHeader header;
    uint64_t     timestamp_ms;     // echoed from ping

    PongPacket() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::PONG;
    }
};

#pragma pack(pop)

} // namespace kmp
