// player_sync.cpp — Synchronize local player state with remote server
//
// Each frame (via game_hooks):
//   1. Read local player position/rotation/animation
//   2. If changed significantly, send PLAYER_STATE to server
//   3. Poll network for incoming packets
//   4. Dispatch received packets to npc_manager / ui

#include <cmath>
#include <cstring>

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

extern void* game_get_player_character();
extern bool  game_is_world_loaded();

extern void npc_manager_on_spawn(const SpawnNPC& pkt);
extern void npc_manager_on_state(const PlayerState& pkt);
extern void npc_manager_on_disconnect(uint32_t player_id);
extern void npc_manager_update(float dt);

extern void ui_on_chat(const ChatMessage& pkt);
extern void ui_on_connect_accept(uint32_t player_id);

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

// Read the local player's current state from the game via KenshiLib
static bool read_local_player_state(PlayerState& out) {
    void* character = game_get_player_character();
    if (!character) return false;

    // TODO: Read from KenshiLib Character object
    // Typical pattern:
    //   Character* ch = static_cast<Character*>(character);
    //   auto pos = ch->getPosition();   // Ogre::Vector3
    //   auto rot = ch->getRotation();   // Ogre::Quaternion or yaw float
    //
    //   out.x = pos.x;
    //   out.y = pos.y;
    //   out.z = pos.z;
    //   out.yaw = rot.getYaw().valueRadians();
    //   out.animation_id = ch->getCurrentAnimation();
    //   out.speed = ch->getSpeed();

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

    case PacketType::PONG: {
        // TODO: calculate RTT for display
        break;
    }

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
// Tick — called every frame from game_hooks
// ---------------------------------------------------------------------------
void player_sync_tick() {
    if (!s_initialized || !client_is_connected()) return;
    if (!game_is_world_loaded()) return;

    // Approximate dt — ideally get from Ogre or game timer
    // For now use tick interval as approximation
    float dt = TICK_INTERVAL_SEC;

    // Poll network
    client_poll();

    // Update remote NPC positions (interpolation)
    npc_manager_update(dt);

    // Send local player state at tick rate
    s_send_timer += dt;
    if (s_send_timer >= TICK_INTERVAL_SEC) {
        s_send_timer = 0.0f;

        PlayerState current;
        if (read_local_player_state(current)) {
            // Only send if position changed significantly
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
