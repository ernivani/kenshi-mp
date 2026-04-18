// session.cpp — Player session management
//
// Tracks connected players, assigns IDs, handles connect/disconnect/timeout.

#include <map>
#include <deque>
#include <chrono>
#include <cstring>
#include <vector>
#include <string>

#include <enet/enet.h>
#include <spdlog/spdlog.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"
#include "posture.h"
#include "session_api.h"
#include "events.h"

namespace kmp {

// External
void relay_broadcast(ENetPeer* exclude, const uint8_t* data, size_t length, bool reliable);
void relay_send_to(ENetPeer* peer, const uint8_t* data, size_t length, bool reliable);
void world_state_add_player(uint32_t id, const char* name, const char* model);
void world_state_remove_player(uint32_t id);
void world_state_update_position(uint32_t id, float x, float y, float z, float yaw,
                                  uint32_t anim, float speed);

// ---------------------------------------------------------------------------
// Session data
// ---------------------------------------------------------------------------
struct PlayerSession {
    uint32_t    id;
    ENetPeer*   peer;
    char        name[MAX_NAME_LENGTH];
    char        model[MAX_MODEL_LENGTH];
    float       x, y, z;
    float       yaw;
    float       speed;
    bool        is_host;
    uint32_t    last_animation_id;
    uint8_t     last_posture_flags;
    std::chrono::steady_clock::time_point last_activity;
};

static std::map<uint32_t, PlayerSession> s_sessions;        // id -> session
static std::map<ENetPeer*, uint32_t>     s_peer_to_id;      // peer -> id
static uint32_t s_next_id = 1;
static uint32_t s_host_id = 0;  // player ID of the host (0 = no host)

// Chat + posture log rings (GUI panes consume these).
static constexpr size_t CHAT_RING_MAX     = 512;
static constexpr size_t POSTURE_RING_MAX  = 512;
static std::deque<ChatLogEntry>      s_chat_log;
static std::deque<PostureTransition> s_posture_log;

static uint64_t now_wall_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void session_init() {
    s_sessions.clear();
    s_peer_to_id.clear();
    s_next_id = 1;
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------
void session_on_connect(ENetPeer* peer) {
    // Peer connected at ENet level — wait for CONNECT_REQUEST packet
    // Store peer temporarily (no session yet)
    peer->data = nullptr;
}

// ---------------------------------------------------------------------------
// Disconnect
// ---------------------------------------------------------------------------
void session_on_disconnect(ENetPeer* peer) {
    auto it = s_peer_to_id.find(peer);
    if (it == s_peer_to_id.end()) return;

    uint32_t id = it->second;
    if (s_host_id == id) {
        s_host_id = 0;
        spdlog::info("Host player disconnected");
    }
    spdlog::info("Player {} ('{}') disconnected", id, s_sessions[id].name);
    events_emit_player_disconnected(id, s_sessions[id].name);

    // Notify all other clients
    PlayerDisconnect pkt;
    pkt.player_id = id;
    auto buf = pack(pkt);
    relay_broadcast(peer, buf.data(), buf.size(), true);

    world_state_remove_player(id);
    s_sessions.erase(id);
    s_peer_to_id.erase(it);
}

// ---------------------------------------------------------------------------
// Packet handling
// ---------------------------------------------------------------------------
static void handle_connect_request(ENetPeer* peer, const uint8_t* data, size_t length) {
    ConnectRequest req;
    if (!unpack(data, length, req)) return;

    // Check max players
    if (s_sessions.size() >= MAX_PLAYERS) {
        ConnectReject reject;
        safe_strcpy(reject.reason, "Server is full");
        auto buf = pack(reject);
        relay_send_to(peer, buf.data(), buf.size(), true);
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // Create session
    uint32_t id = s_next_id++;
    PlayerSession session;
    session.id = id;
    session.peer = peer;
    std::strncpy(session.name, req.name, MAX_NAME_LENGTH - 1);
    session.name[MAX_NAME_LENGTH - 1] = '\0';
    std::strncpy(session.model, req.model, MAX_MODEL_LENGTH - 1);
    session.model[MAX_MODEL_LENGTH - 1] = '\0';
    session.x = session.y = session.z = session.yaw = 0.0f;
    session.speed = 0.0f;
    session.last_animation_id = 0;
    session.last_posture_flags = 0;
    session.last_activity = std::chrono::steady_clock::now();
    session.is_host = (req.is_host != 0);
    if (session.is_host && s_host_id == 0) {
        s_host_id = id;
        spdlog::info("Player {} is the HOST", id);
    }

    s_sessions[id] = session;
    s_peer_to_id[peer] = id;

    spdlog::info("Player {} ('{}') joined with model '{}'", id, session.name, session.model);
    events_emit_player_connected(id, session.name);

    // Send accept
    ConnectAccept accept;
    accept.player_id = id;
    auto buf_accept = pack(accept);
    relay_send_to(peer, buf_accept.data(), buf_accept.size(), true);

    // Tell the new player about all existing players
    for (auto& pair : s_sessions) {
        if (pair.first == id) continue;

        SpawnNPC spawn;
        spawn.player_id = pair.first;
        safe_strcpy(spawn.name, pair.second.name);
        safe_strcpy(spawn.model, pair.second.model);
        spawn.x = pair.second.x;
        spawn.y = pair.second.y;
        spawn.z = pair.second.z;
        spawn.yaw = pair.second.yaw;

        auto buf = pack(spawn);
        relay_send_to(peer, buf.data(), buf.size(), true);
    }

    // Tell all existing players about the new player
    SpawnNPC spawn;
    spawn.player_id = id;
    safe_strcpy(spawn.name, session.name);
    safe_strcpy(spawn.model, session.model);
    spawn.x = spawn.y = spawn.z = spawn.yaw = 0.0f;

    auto buf_spawn = pack(spawn);
    relay_broadcast(peer, buf_spawn.data(), buf_spawn.size(), true);

    world_state_add_player(id, session.name, session.model);
}

static void handle_player_state(ENetPeer* peer, const uint8_t* data, size_t length) {
    auto it = s_peer_to_id.find(peer);
    if (it == s_peer_to_id.end()) return;

    PlayerState state;
    if (!unpack(data, length, state)) return;

    uint32_t id = it->second;
    auto& session = s_sessions[id];

    // Update server-side position
    session.x = state.x;
    session.y = state.y;
    session.z = state.z;
    session.yaw = state.yaw;
    session.speed = state.speed;
    session.last_activity = std::chrono::steady_clock::now();

    // Track posture flag transitions (low 8 bits of animation_id).
    uint8_t new_flags = posture_flags_from_anim(state.animation_id);
    if (new_flags != session.last_posture_flags) {
        PostureTransition t;
        t.player_id   = id;
        t.player_name = session.name;
        t.old_flags   = session.last_posture_flags;
        t.new_flags   = new_flags;
        t.time_ms     = now_wall_ms();
        s_posture_log.push_back(std::move(t));
        if (s_posture_log.size() > POSTURE_RING_MAX) s_posture_log.pop_front();
        spdlog::debug("Player {} posture {} -> {}",
            id,
            posture_short_label(session.last_posture_flags),
            posture_short_label(new_flags));
        events_emit_posture(id, session.name, session.last_posture_flags, new_flags);
    }
    session.last_animation_id  = state.animation_id;
    session.last_posture_flags = new_flags;

    // Stamp the correct player_id (don't trust client)
    PlayerState relayed = state;
    relayed.player_id = id;

    auto buf = pack(relayed);
    relay_broadcast(peer, buf.data(), buf.size(), false);

    world_state_update_position(id, state.x, state.y, state.z,
                                 state.yaw, state.animation_id, state.speed);
}

static void handle_chat_message(ENetPeer* peer, const uint8_t* data, size_t length) {
    auto it = s_peer_to_id.find(peer);
    if (it == s_peer_to_id.end()) return;

    ChatMessage msg;
    if (!unpack(data, length, msg)) return;

    uint32_t id = it->second;
    spdlog::info("[Chat] Player {}: {}", id, msg.message);

    session_chat_push(id, s_sessions[id].name, msg.message);
    events_emit_chat(id, s_sessions[id].name, msg.message);

    // Stamp correct player_id and broadcast to all (including sender)
    ChatMessage relayed = msg;
    relayed.player_id = id;

    auto buf = pack(relayed);
    // Broadcast to all including sender so they see their message confirmed
    relay_broadcast(nullptr, buf.data(), buf.size(), true);
}

static void handle_ping(ENetPeer* peer, const uint8_t* data, size_t length) {
    PingPacket ping;
    if (!unpack(data, length, ping)) return;

    PongPacket pong;
    pong.timestamp_ms = ping.timestamp_ms;
    auto buf = pack(pong);
    relay_send_to(peer, buf.data(), buf.size(), false);
}

static void handle_npc_packet(ENetPeer* peer, const uint8_t* data, size_t length, bool reliable) {
    auto it = s_peer_to_id.find(peer);
    if (it == s_peer_to_id.end()) return;

    uint32_t id = it->second;

    // Only accept NPC packets from the host
    if (id != s_host_id) {
        // spdlog::warn("Non-host player {} tried to send NPC packet, ignoring", id);
        return;
    }

    // Update activity timestamp
    s_sessions[id].last_activity = std::chrono::steady_clock::now();

    spdlog::debug("NPC packet from host (player {}), relaying {} bytes, reliable={}", id, length, reliable);

    // Relay to all non-host players
    relay_broadcast(peer, data, length, reliable);
}

static void handle_combat_to_host(ENetPeer* peer, const uint8_t* data, size_t length) {
    auto it = s_peer_to_id.find(peer);
    if (it == s_peer_to_id.end()) return;

    if (s_host_id == 0) return;

    // Find host peer and forward
    for (auto& pair : s_sessions) {
        if (pair.first == s_host_id) {
            relay_send_to(pair.second.peer, data, length, true);
            break;
        }
    }

    s_sessions[it->second].last_activity = std::chrono::steady_clock::now();
}

void session_on_packet(ENetPeer* peer, const uint8_t* data, size_t length) {
    PacketHeader header;
    if (!peek_header(data, length, header)) return;
    if (!validate_version(header)) return;

    switch (header.type) {
    case PacketType::CONNECT_REQUEST:
        handle_connect_request(peer, data, length);
        break;
    case PacketType::PLAYER_STATE:
        handle_player_state(peer, data, length);
        break;
    case PacketType::CHAT_MESSAGE:
        handle_chat_message(peer, data, length);
        break;
    case PacketType::PING:
        handle_ping(peer, data, length);
        break;
    case PacketType::NPC_SPAWN_REMOTE:
        handle_npc_packet(peer, data, length, true);
        break;
    case PacketType::NPC_BATCH_STATE:
        handle_npc_packet(peer, data, length, false);
        break;
    case PacketType::NPC_DESPAWN_REMOTE:
        handle_npc_packet(peer, data, length, true);
        break;
    case PacketType::BUILDING_SPAWN_REMOTE:
        handle_npc_packet(peer, data, length, true);
        break;
    case PacketType::BUILDING_DESPAWN_REMOTE:
        handle_npc_packet(peer, data, length, true);
        break;
    case PacketType::COMBAT_ATTACK:
        handle_combat_to_host(peer, data, length);
        break;
    case PacketType::COMBAT_DAMAGE:
        handle_npc_packet(peer, data, length, true);
        break;
    case PacketType::PLAYER_COMBAT_STATS:
        handle_combat_to_host(peer, data, length);
        break;
    case PacketType::COMBAT_TARGET:
        handle_combat_to_host(peer, data, length);
        break;
    default:
        spdlog::warn("Unknown packet type: 0x{:02x}", static_cast<uint8_t>(header.type));
        break;
    }
}

// ---------------------------------------------------------------------------
// Timeout check
// ---------------------------------------------------------------------------
void session_check_timeouts() {
    auto now = std::chrono::steady_clock::now();
    std::vector<ENetPeer*> to_disconnect;

    for (auto& pair : s_sessions) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - pair.second.last_activity
        ).count();

        if (elapsed > CLIENT_TIMEOUT_MS) {
            spdlog::warn("Player {} ('{}') timed out", pair.first, pair.second.name);
            to_disconnect.push_back(pair.second.peer);
        }
    }

    for (auto* peer : to_disconnect) {
        enet_peer_disconnect(peer, 0);
        session_on_disconnect(peer);
    }
}

// ---------------------------------------------------------------------------
// GUI / admin-facing snapshot API
// ---------------------------------------------------------------------------
void session_get_players(std::vector<PlayerInfo>& out) {
    out.clear();
    out.reserve(s_sessions.size());
    auto now = std::chrono::steady_clock::now();
    for (auto& pair : s_sessions) {
        const auto& s = pair.second;
        PlayerInfo p;
        p.id      = s.id;
        p.name    = s.name;
        p.model   = s.model;
        p.is_host = s.is_host;
        p.x       = s.x;
        p.y       = s.y;
        p.z       = s.z;
        p.yaw     = s.yaw;
        p.speed   = s.speed;
        p.last_animation_id  = s.last_animation_id;
        p.last_posture_flags = s.last_posture_flags;
        if (s.peer) {
            char addr[64] = {0};
            enet_address_get_host_ip(&s.peer->address, addr, sizeof(addr));
            p.address = std::string(addr) + ":" + std::to_string(s.peer->address.port);
            p.ping_ms = s.peer->roundTripTime;
        } else {
            p.address = "";
            p.ping_ms = 0;
        }
        p.idle_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - s.last_activity).count());
        out.push_back(std::move(p));
    }
}

ENetPeer* session_find_peer(uint32_t player_id) {
    auto it = s_sessions.find(player_id);
    if (it == s_sessions.end()) return nullptr;
    return it->second.peer;
}

bool session_get_player_snapshot(uint32_t player_id, PlayerInfo& out) {
    auto it = s_sessions.find(player_id);
    if (it == s_sessions.end()) return false;
    const auto& s = it->second;
    out.id      = s.id;
    out.name    = s.name;
    out.model   = s.model;
    out.is_host = s.is_host;
    out.x       = s.x;
    out.y       = s.y;
    out.z       = s.z;
    out.yaw     = s.yaw;
    out.speed   = s.speed;
    out.last_animation_id  = s.last_animation_id;
    out.last_posture_flags = s.last_posture_flags;
    out.address = "";
    out.ping_ms = 0;
    out.idle_ms = 0;
    return true;
}

void session_chat_push(uint32_t player_id, const std::string& author, const std::string& text) {
    ChatLogEntry e;
    e.player_id = player_id;
    e.author    = author;
    e.text      = text;
    e.time_ms   = now_wall_ms();
    s_chat_log.push_back(std::move(e));
    if (s_chat_log.size() > CHAT_RING_MAX) s_chat_log.pop_front();
}

void session_chat_snapshot(std::vector<ChatLogEntry>& out) {
    out.assign(s_chat_log.begin(), s_chat_log.end());
}

void session_posture_snapshot(std::vector<PostureTransition>& out) {
    out.assign(s_posture_log.begin(), s_posture_log.end());
}

} // namespace kmp
