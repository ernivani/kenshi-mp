#pragma once

#include <cstdint>
#include <cstring>
#include "protocol.h"

namespace kmp {

// ---------------------------------------------------------------------------
// Packet types
// ---------------------------------------------------------------------------
namespace PacketType {
    static const uint8_t CONNECT_REQUEST  = 0x01;
    static const uint8_t CONNECT_ACCEPT   = 0x02;
    static const uint8_t CONNECT_REJECT   = 0x03;
    static const uint8_t PLAYER_STATE     = 0x10;
    static const uint8_t PLAYER_DISCONNECT= 0x11;
    static const uint8_t SPAWN_NPC        = 0x20;
    static const uint8_t CHAT_MESSAGE     = 0x30;
    static const uint8_t PING             = 0xF0;
    static const uint8_t PONG             = 0xF1;
    static const uint8_t NPC_BATCH_STATE    = 0x40;
    static const uint8_t NPC_SPAWN_REMOTE   = 0x41;
    static const uint8_t NPC_DESPAWN_REMOTE = 0x42;
    static const uint8_t COMBAT_ATTACK      = 0x50;
    static const uint8_t COMBAT_DAMAGE      = 0x51;
}

// ---------------------------------------------------------------------------
// Packet header — prefixed to every packet
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

struct PacketHeader {
    uint8_t    version;    // PROTOCOL_VERSION
    uint8_t    type;       // PacketType::*
};

// ---------------------------------------------------------------------------
// Packet payloads
// ---------------------------------------------------------------------------

struct ConnectRequest {
    PacketHeader header;
    char         name[MAX_NAME_LENGTH];
    char         model[MAX_MODEL_LENGTH];
    uint8_t      is_host;

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

struct NPCSpawnRemote {
    PacketHeader header;
    uint32_t     npc_id;
    char         name[MAX_NAME_LENGTH];
    char         race[MAX_RACE_LENGTH];
    char         weapon[MAX_WEAPON_LENGTH];
    char         armour[MAX_ARMOUR_LENGTH];
    float        x, y, z;
    float        yaw;

    NPCSpawnRemote() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::NPC_SPAWN_REMOTE;
    }
};

struct NPCDespawnRemote {
    PacketHeader header;
    uint32_t     npc_id;

    NPCDespawnRemote() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::NPC_DESPAWN_REMOTE;
    }
};

struct NPCStateEntry {
    uint32_t npc_id;
    float    x, y, z;
    float    yaw;
    float    speed;
    uint32_t animation_id;
    uint8_t  flags;            // bit 0: isDown, bit 1: isDead
    int16_t  health_percent;   // 0-100
};

struct NPCBatchHeader {
    PacketHeader header;
    uint16_t     count;

    NPCBatchHeader() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::NPC_BATCH_STATE;
    }
};

struct CombatAttack {
    PacketHeader header;
    uint32_t     target_npc_id;
    float        cut_damage;
    float        blunt_damage;
    float        pierce_damage;

    CombatAttack() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::COMBAT_ATTACK;
    }
};

struct CombatDamage {
    PacketHeader header;
    uint32_t     player_id;
    float        cut_damage;
    float        blunt_damage;
    float        pierce_damage;

    CombatDamage() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::COMBAT_DAMAGE;
    }
};

#pragma pack(pop)

} // namespace kmp
