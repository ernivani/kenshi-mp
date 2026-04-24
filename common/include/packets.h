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
    // Host uploads the current save snapshot to the server. Multi-chunk,
    // sha256-verified. See docs/superpowers/specs/2026-04-21-save-transfer-and-load-design.md
    static const uint8_t SNAPSHOT_UPLOAD_BEGIN = 0xA0;
    static const uint8_t SNAPSHOT_UPLOAD_CHUNK = 0xA1;
    static const uint8_t SNAPSHOT_UPLOAD_END   = 0xA2;
    static const uint8_t SNAPSHOT_UPLOAD_ACK   = 0xA3;
    // Server browser: unauth ping. Client sends REQUEST to a server it's
    // never connected to; server responds with REPLY before CONNECT_REQUEST
    // gate. See docs/superpowers/specs/2026-04-22-server-browser-ui-design.md
    static const uint8_t SERVER_INFO_REQUEST = 0xB0;
    static const uint8_t SERVER_INFO_REPLY   = 0xB1;
    // Per-server character persistence. Joiner serializes its
    // PlayerInterface squad state and uploads to the server; the server
    // persists by client_uuid. On reconnect, server sends the stored
    // blob back so the joiner restores the exact same squad instead of
    // spawning a fresh Wanderer.
    static const uint8_t CHARACTER_UPLOAD    = 0xC0;
    static const uint8_t CHARACTER_RESTORE   = 0xC1;
    // Per-character appearance blob (Kenshi GameDataCopyStandalone bytes
    // from Character::getAppearanceData() → saveToFile). Clients send on
    // local spawn + after editor close; server relays to all peers AND
    // caches. New joiners receive cached blobs before any SpawnNPC so
    // the blob is ready at spawn time (enables giveBirth-style spawn
    // instead of random).
    static const uint8_t CHARACTER_APPEARANCE = 0xC2;
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
    char         password[MAX_PASSWORD_LENGTH];   // "" = no password provided

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

struct SnapshotUploadBegin {
    PacketHeader header;
    uint32_t     upload_id;
    uint32_t     rev;
    uint64_t     total_size;
    uint8_t      sha256[32];

    SnapshotUploadBegin() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_BEGIN;
    }
};

// Wire layout: {PacketHeader, upload_id, offset, length}{<length> bytes}.
// Serialize via pack_with_tail / deserialize via unpack_with_tail.
struct SnapshotUploadChunk {
    PacketHeader header;
    uint32_t     upload_id;
    uint32_t     offset;
    uint16_t     length;

    SnapshotUploadChunk() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_CHUNK;
    }
};

struct SnapshotUploadEnd {
    PacketHeader header;
    uint32_t     upload_id;

    SnapshotUploadEnd() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_END;
    }
};

struct SnapshotUploadAck {
    PacketHeader header;
    uint32_t     upload_id;
    uint8_t      accepted;    // 0 = rejected, 1 = accepted
    uint8_t      error_code;  // 0=none, 1=sha_mismatch, 2=size_mismatch, 3=out_of_order, 4=no_upload
    uint16_t     _pad;

    SnapshotUploadAck() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_ACK;
    }
};

struct ServerInfoRequest {
    PacketHeader header;
    uint32_t     nonce;

    ServerInfoRequest() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SERVER_INFO_REQUEST;
    }
};

struct ServerInfoReply {
    PacketHeader header;
    uint32_t     nonce;
    uint16_t     player_count;
    uint16_t     max_players;
    uint8_t      protocol_version;
    uint8_t      password_required;  // 0 or 1
    uint8_t      _pad[2];
    char         description[128];   // null-terminated, UTF-8

    ServerInfoReply() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SERVER_INFO_REPLY;
    }
};

// Joiner-side → server: upload the serialized PlayerInterface blob
// (Kenshi's GameData binary form) so the server can persist the
// joiner's squad across sessions. Wire: {CharacterUpload}{<blob_size> bytes}.
// Send via pack_with_tail, unpack with unpack_with_tail.
struct CharacterUpload {
    PacketHeader header;
    uint32_t     blob_size;

    CharacterUpload() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CHARACTER_UPLOAD;
    }
};

// Server → joiner: after CONNECT_ACCEPT, if the server has a blob
// stored for this UUID, send it so the client restores the squad
// instead of spawning a fresh one. Same wire layout as CharacterUpload.
struct CharacterRestore {
    PacketHeader header;
    uint32_t     blob_size;

    CharacterRestore() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CHARACTER_RESTORE;
    }
};

// Per-character appearance blob (authoritative player_id = server's
// session id). Sent by clients, relayed+cached by the server, received
// by all other peers to drive correct remote-NPC skin. Wire:
// {CharacterAppearance}{<blob_size> bytes}.
struct CharacterAppearance {
    PacketHeader header;
    uint32_t     player_id;
    uint32_t     blob_size;

    CharacterAppearance() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CHARACTER_APPEARANCE;
    }
};

#pragma pack(pop)

} // namespace kmp
