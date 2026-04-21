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
    static const uint8_t PLAYER_COMBAT_STATS = 0x55;
    static const uint8_t COMBAT_TARGET      = 0x56;
    static const uint8_t BUILDING_SPAWN_REMOTE   = 0x60;
    static const uint8_t BUILDING_DESPAWN_REMOTE = 0x62;
    // Server-authored spawns: broadcast to ALL peers (host + joiners).
    // Wire layouts match NPC_SPAWN_REMOTE / NPC_DESPAWN_REMOTE /
    // BUILDING_SPAWN_REMOTE / BUILDING_DESPAWN_REMOTE respectively.
    static const uint8_t SERVER_SPAWN_NPC        = 0x70;
    static const uint8_t SERVER_DESPAWN_NPC      = 0x71;
    static const uint8_t SERVER_SPAWN_BUILDING   = 0x72;
    static const uint8_t SERVER_DESPAWN_BUILDING = 0x73;
    // Host-originated admin command: move a target player to a location.
    // Host → server → target-peer; target client applies it to its own char.
    static const uint8_t FORCE_TELEPORT          = 0x80;
    // Host enumerates its GameData BUILDING entries on connect and streams
    // them to the server so the admin GUI can show a proper dropdown.
    static const uint8_t BUILDING_CATALOG_ENTRY  = 0x90;
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
    // Stable per-installation identity. The client generates a UUID once (at
    // first run) and persists it; the server maps uuid → player_id so a player
    // who reconnects gets the same id as before.
    char         client_uuid[64];

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

// Posture flags packed into PlayerState::animation_id (Phase A).
// Only the low 8 bits are used; upper bits reserved for future anim-id use.
static const uint32_t POSTURE_DOWN        = 1u << 0;
static const uint32_t POSTURE_UNCONSCIOUS = 1u << 1;
static const uint32_t POSTURE_RAGDOLL     = 1u << 2;
static const uint32_t POSTURE_DEAD        = 1u << 3;
static const uint32_t POSTURE_CHAINED     = 1u << 4;
// Any flag in this mask means the avatar should be ragdolled on receivers.
static const uint32_t POSTURE_RAGDOLL_MASK =
    POSTURE_DOWN | POSTURE_UNCONSCIOUS | POSTURE_RAGDOLL | POSTURE_DEAD;

struct PlayerState {
    PacketHeader header;
    uint32_t     player_id;
    float        x, y, z;          // world position
    float        yaw;              // rotation around Y axis (radians)
    uint32_t     animation_id;     // low 8 bits: posture flags (POSTURE_*)
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

// Spawn flag bits. Currently only used for SERVER_SPAWN_NPC (from admin GUI);
// host→joiner sync leaves spawn_flags = 0 which keeps the legacy "AI cleared"
// behaviour (the receiver calls neutralize_remote_avatar).
static const uint8_t NPC_SPAWN_FLAG_ENABLE_AI = 0x01;

struct NPCSpawnRemote {
    PacketHeader header;
    uint32_t     npc_id;
    char         name[MAX_NAME_LENGTH];
    char         race[MAX_RACE_LENGTH];
    char         weapon[MAX_WEAPON_LENGTH];
    char         armour[MAX_ARMOUR_LENGTH];
    float        x, y, z;
    float        yaw;
    uint8_t      spawn_flags;

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

struct PlayerCombatStats {
    PacketHeader header;
    uint32_t     player_id;
    float        strength;
    float        dexterity;
    float        toughness;
    float        melee_attack;
    float        melee_defence;
    float        athletics;

    PlayerCombatStats() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::PLAYER_COMBAT_STATS;
    }
};

struct CombatTarget {
    PacketHeader header;
    uint32_t     player_id;
    uint32_t     target_npc_id;

    CombatTarget() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::COMBAT_TARGET;
    }
};

struct BuildingSpawnRemote {
    PacketHeader header;
    uint32_t     building_id;
    char         stringID[MAX_STRINGID_LENGTH];
    float        x, y, z;
    float        qw, qx, qy, qz;
    uint8_t      completed;
    uint8_t      is_foliage;
    int16_t      floor;

    BuildingSpawnRemote() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::BUILDING_SPAWN_REMOTE;
    }
};

struct BuildingDespawnRemote {
    PacketHeader header;
    uint32_t     building_id;

    BuildingDespawnRemote() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::BUILDING_DESPAWN_REMOTE;
    }
};

// Admin: force a target player to a position. Sent by host client → server;
// server forwards to the target peer if the sender is the host.
struct ForceTeleport {
    PacketHeader header;
    uint32_t     target_player_id;
    float        x, y, z;

    ForceTeleport() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::FORCE_TELEPORT;
    }
};

// One entry in the host's GameData building catalog.
struct BuildingCatalogEntry {
    PacketHeader header;
    char         stringID[64];
    char         name[64];

    BuildingCatalogEntry() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::BUILDING_CATALOG_ENTRY;
    }
};

#pragma pack(pop)

} // namespace kmp
