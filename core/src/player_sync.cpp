// player_sync.cpp — Synchronize local player state with remote server
//
// Each frame (via game loop hook):
//   1. Poll network for incoming packets
//   2. Read local player position/rotation from KenshiLib
//   3. If changed significantly, send PLAYER_STATE to server (20Hz)
//   4. Update remote NPC interpolation

#include <cmath>
#include <cstring>
#include <functional>

#include <kenshi/Character.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreMath.h>
#include <OgreLogManager.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

// External subsystems
extern void client_poll();
extern void client_send_unreliable(const uint8_t* data, size_t length);
extern void client_send_reliable(const uint8_t* data, size_t length);
extern bool client_is_connected();
extern uint32_t client_get_local_id();
extern void client_set_local_id(uint32_t id);
extern void client_set_packet_callback(std::function<void(const uint8_t*, size_t)> cb);

extern Character* game_get_player_character();
extern bool game_is_world_loaded();

extern void npc_manager_on_spawn(const SpawnNPC& pkt);
extern void npc_manager_on_state(const PlayerState& pkt);
extern void npc_manager_on_disconnect(uint32_t player_id);
extern void npc_manager_update(float dt);

extern void ui_on_chat(const ChatMessage& pkt);
extern void ui_on_connect_accept(uint32_t player_id);
extern void ui_check_hotkey();

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static PlayerState s_last_sent_state;
static float       s_send_timer = 0.0f;
static bool        s_initialized = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static float distance_sq(const PlayerState& a, const PlayerState& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx*dx + dy*dy + dz*dz;
}

// Read the local player's current state from KenshiLib
static bool read_local_player_state(PlayerState& out) {
    Character* ch = game_get_player_character();
    if (!ch) return false;

    Ogre::Vector3 pos = ch->getPosition();
    out.x = pos.x;
    out.y = pos.y;
    out.z = pos.z;

    // Extract yaw from the character's raw position data
    out.yaw = 0.0f;
    float speed = ch->getMovementSpeed();
    out.speed = speed;

    out.animation_id = 0;  // MVP: idle only, animation sync is future work
    out.player_id = client_get_local_id();

    return true;
}

// ---------------------------------------------------------------------------
// Packet dispatch
// ---------------------------------------------------------------------------
static void on_packet_received(const uint8_t* data, size_t length) {
    auto header = peek_header(data, length);
    if (!header || !validate_version(*header)) return;

    switch (header->type) {
    case PacketType::CONNECT_ACCEPT: {
        auto pkt = unpack<ConnectAccept>(data, length);
        if (pkt) {
            client_set_local_id(pkt->player_id);
            ui_on_connect_accept(pkt->player_id);
        }
        break;
    }

    case PacketType::SPAWN_NPC: {
        auto pkt = unpack<SpawnNPC>(data, length);
        if (pkt) {
            npc_manager_on_spawn(*pkt);
        }
        break;
    }

    case PacketType::PLAYER_STATE: {
        auto pkt = unpack<PlayerState>(data, length);
        if (pkt && pkt->player_id != client_get_local_id()) {
            npc_manager_on_state(*pkt);
        }
        break;
    }

    case PacketType::PLAYER_DISCONNECT: {
        auto pkt = unpack<PlayerDisconnect>(data, length);
        if (pkt) {
            npc_manager_on_disconnect(pkt->player_id);
        }
        break;
    }

    case PacketType::CHAT_MESSAGE: {
        auto pkt = unpack<ChatMessage>(data, length);
        if (pkt) {
            ui_on_chat(*pkt);
        }
        break;
    }

    case PacketType::PONG:
        // TODO: calculate RTT for display
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void player_sync_init() {
    std::memset(&s_last_sent_state, 0, sizeof(s_last_sent_state));
    s_send_timer = 0.0f;
    client_set_packet_callback(on_packet_received);
    s_initialized = true;
}

void player_sync_shutdown() {
    s_initialized = false;
}

// ---------------------------------------------------------------------------
// Tick — called every frame from the game loop hook with real delta time
// ---------------------------------------------------------------------------
void player_sync_tick(float dt) {
    if (!s_initialized) return;

    // Log periodically to confirm tick is running
    static int s_tick_count = 0;
    s_tick_count++;
    if (s_tick_count <= 3 || s_tick_count % 1000 == 0) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] Tick #" + std::to_string(s_tick_count));
    }

    // Always check for F8/F9 hotkey
    ui_check_hotkey();

    // Poll network if connected (to receive CONNECT_ACCEPT, etc.)
    if (client_is_connected()) {
        client_poll();
    }

    // Only do game sync when world is loaded
    if (!client_is_connected() || !game_is_world_loaded()) return;

    // Update remote NPC positions (interpolation)
    npc_manager_update(dt);

    // Send local player state at tick rate
    s_send_timer += dt;
    if (s_send_timer >= TICK_INTERVAL_SEC) {
        s_send_timer = 0.0f;

        PlayerState current;
        if (read_local_player_state(current)) {
            if (distance_sq(current, s_last_sent_state) > POSITION_EPSILON * POSITION_EPSILON ||
                current.animation_id != s_last_sent_state.animation_id) {

                auto buf = pack(current);
                client_send_unreliable(buf.data(), buf.size());
                s_last_sent_state = current;
            }
        }
    }
}

} // namespace kmp
