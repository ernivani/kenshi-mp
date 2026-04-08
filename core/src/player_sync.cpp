// player_sync.cpp — Synchronize local player state with remote server
//
// Runs on the game thread via AddHook. Reads player state from KenshiLib,
// sends over ENet, dispatches incoming packets to subsystems.

#include <cmath>
#include <cstring>
#include <string>
#include <sstream>

#include <kenshi/Character.h>
#include <OgreVector3.h>
#include <OgreLogManager.h>
#include "kmp_log.h"

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

static std::string itos(uint32_t val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

// External subsystems
extern void client_poll();
extern bool client_connect(const char* host, uint16_t port);
extern void client_disconnect();
extern void client_send_unreliable(const uint8_t* data, size_t length);
extern void client_send_reliable(const uint8_t* data, size_t length);
extern bool client_is_connected();
extern uint32_t client_get_local_id();
extern void client_set_local_id(uint32_t id);
typedef void (*PacketCallback)(const uint8_t* data, size_t length);
extern void client_set_packet_callback(PacketCallback cb);

extern Character* game_get_player_character();
extern bool game_is_world_loaded();

extern void npc_manager_on_spawn(const SpawnNPC& pkt);
extern void npc_manager_on_state(const PlayerState& pkt);
extern void npc_manager_on_disconnect(uint32_t player_id);
extern void npc_manager_on_remote_spawn(const NPCSpawnRemote& pkt);
extern void npc_manager_on_remote_state(const NPCStateEntry& entry);
extern void npc_manager_on_remote_despawn(uint32_t npc_id);
extern void npc_manager_update(float dt);
extern void npc_manager_hide_local_npcs();
extern void npc_manager_show_local_npcs();

extern void host_sync_init();
extern void host_sync_shutdown();
extern void host_sync_tick(float dt);
extern void host_sync_set_host(bool is_host);
extern void host_sync_resend_all();
extern bool host_sync_is_host();

extern void admin_panel_init();
extern void admin_panel_shutdown();
extern void admin_panel_check_hotkey();
extern void admin_panel_update(float dt);
extern void admin_panel_on_player_state(uint32_t player_id, float x, float y, float z);
extern void admin_panel_on_player_disconnect(uint32_t player_id);

extern void ui_on_chat(const ChatMessage& pkt);
extern void ui_on_connect_accept(uint32_t player_id);
extern void ui_on_disconnect();
extern void ui_check_hotkey();

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static PlayerState s_last_sent_state;
static float       s_send_timer = 0.0f;
static bool        s_initialized = false;
static bool        s_requested_host = false;  // did we connect with is_host=1?

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static float distance_sq(const PlayerState& a, const PlayerState& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx*dx + dy*dy + dz*dz;
}

static bool read_local_player_state(PlayerState& out) {
    Character* ch = game_get_player_character();
    if (!ch) return false;

    Ogre::Vector3 pos = ch->getPosition();
    out.x = pos.x;
    out.y = pos.y;
    out.z = pos.z;
    out.yaw = 0.0f;
    out.speed = ch->getMovementSpeed();
    out.animation_id = 0;
    out.player_id = client_get_local_id();

    return true;
}

// ---------------------------------------------------------------------------
// Packet dispatch
// ---------------------------------------------------------------------------
static void on_packet_received(const uint8_t* data, size_t length) {
    PacketHeader header;
    if (!peek_header(data, length, header)) return;
    if (!validate_version(header)) return;

    // Log NPC-related packets
    if (header.type >= 0x40 && header.type <= 0x42) {
        KMP_LOG("[KenshiMP] Received packet type=0x" + itos(header.type) + " len=" + itos(static_cast<uint32_t>(length)));
    }

    switch (header.type) {
    case PacketType::CONNECT_ACCEPT: {
        ConnectAccept pkt;
        if (unpack(data, length, pkt)) {
            client_set_local_id(pkt.player_id);
            host_sync_set_host(s_requested_host);
            if (!s_requested_host) {
                npc_manager_hide_local_npcs();
            }
            ui_on_connect_accept(pkt.player_id);
        }
        break;
    }

    case PacketType::SPAWN_NPC: {
        SpawnNPC pkt;
        if (unpack(data, length, pkt)) {
            npc_manager_on_spawn(pkt);
            // If we're the host and a new player joined, resend all synced NPCs
            if (host_sync_is_host()) {
                host_sync_resend_all();
            }
        }
        break;
    }

    case PacketType::PLAYER_STATE: {
        PlayerState pkt;
        if (unpack(data, length, pkt)) {
            if (pkt.player_id != client_get_local_id()) {
                npc_manager_on_state(pkt);
                admin_panel_on_player_state(pkt.player_id, pkt.x, pkt.y, pkt.z);
            }
        }
        break;
    }

    case PacketType::PLAYER_DISCONNECT: {
        PlayerDisconnect pkt;
        if (unpack(data, length, pkt)) {
            npc_manager_on_disconnect(pkt.player_id);
            admin_panel_on_player_disconnect(pkt.player_id);
        }
        break;
    }

    case PacketType::CHAT_MESSAGE: {
        ChatMessage pkt;
        if (unpack(data, length, pkt)) {
            ui_on_chat(pkt);
        }
        break;
    }

    case PacketType::PONG:
        break;

    case PacketType::NPC_SPAWN_REMOTE: {
        KMP_LOG("[KenshiMP] Got NPC_SPAWN_REMOTE, is_host=" + std::string(host_sync_is_host() ? "true" : "false"));
        if (!host_sync_is_host()) {
            NPCSpawnRemote pkt;
            if (unpack(data, length, pkt)) {
                KMP_LOG("[KenshiMP] Spawning remote NPC " + itos(pkt.npc_id));
                npc_manager_on_remote_spawn(pkt);
            }
        }
        break;
    }

    case PacketType::NPC_BATCH_STATE: {
        if (!host_sync_is_host()) {
            NPCBatchHeader batch_hdr;
            if (unpack(data, length, batch_hdr)) {
                size_t offset = sizeof(NPCBatchHeader);
                for (uint16_t i = 0; i < batch_hdr.count; ++i) {
                    if (offset + sizeof(NPCStateEntry) > length) break;
                    NPCStateEntry entry;
                    std::memcpy(&entry, data + offset, sizeof(NPCStateEntry));
                    npc_manager_on_remote_state(entry);
                    offset += sizeof(NPCStateEntry);
                }
            }
        }
        break;
    }

    case PacketType::NPC_DESPAWN_REMOTE: {
        if (!host_sync_is_host()) {
            NPCDespawnRemote pkt;
            if (unpack(data, length, pkt)) {
                npc_manager_on_remote_despawn(pkt.npc_id);
            }
        }
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void player_sync_set_requested_host(bool val) {
    s_requested_host = val;
}

void player_sync_init() {
    std::memset(&s_last_sent_state, 0, sizeof(s_last_sent_state));
    s_send_timer = 0.0f;
    client_set_packet_callback(on_packet_received);
    host_sync_init();
    admin_panel_init();
    s_initialized = true;
}

void player_sync_shutdown() {
    s_initialized = false;
}

// ---------------------------------------------------------------------------
// Tick — called every frame on the game thread via AddHook
// ---------------------------------------------------------------------------
void player_sync_tick(float dt) {
    if (!s_initialized) return;

    // Track connection state to detect drops
    static bool s_was_connected = false;
    static bool s_auto_reconnect = false;
    static std::string s_last_host;
    static uint16_t s_last_port = 0;

    // Always check for hotkeys
    ui_check_hotkey();
    admin_panel_check_hotkey();
    admin_panel_update(dt);

    // Poll network if connected
    if (client_is_connected()) {
        client_poll();
        s_was_connected = true;
    }

    // Detect disconnection
    if (s_was_connected && !client_is_connected()) {
        s_was_connected = false;
        s_auto_reconnect = true;
        KMP_LOG("[KenshiMP] Connection lost, will auto-reconnect...");
        npc_manager_show_local_npcs();
        ui_on_disconnect();
    }

    // Auto-reconnect after disconnect
    if (s_auto_reconnect && !client_is_connected() && game_is_world_loaded()) {
        static float s_reconnect_timer = 0.0f;
        s_reconnect_timer += dt;
        if (s_reconnect_timer >= 3.0f) {
            s_reconnect_timer = 0.0f;
            KMP_LOG("[KenshiMP] Attempting reconnect...");
            if (client_connect("127.0.0.1", 7777)) {
                ConnectRequest req;
                std::strncpy(req.name, "Player", MAX_NAME_LENGTH - 1);
                req.name[MAX_NAME_LENGTH - 1] = '\0';
                std::strncpy(req.model, "greenlander", MAX_MODEL_LENGTH - 1);
                req.model[MAX_MODEL_LENGTH - 1] = '\0';
                req.is_host = 1;
                std::vector<uint8_t> buf = pack(req);
                client_send_reliable(buf.data(), buf.size());
                s_auto_reconnect = false;
                KMP_LOG("[KenshiMP] Reconnected!");
            }
        }
    }

    // Only do game sync when connected and world is loaded
    if (!client_is_connected() || !game_is_world_loaded()) return;

    // Update remote NPC positions (interpolation)
    npc_manager_update(dt);

    // Host: scan and send NPC state to server
    host_sync_tick(dt);

    // Send local player state at tick rate
    s_send_timer += dt;
    if (s_send_timer >= TICK_INTERVAL_SEC) {
        s_send_timer = 0.0f;

        PlayerState current;
        if (read_local_player_state(current)) {
            if (distance_sq(current, s_last_sent_state) > POSITION_EPSILON * POSITION_EPSILON ||
                current.animation_id != s_last_sent_state.animation_id) {

                std::vector<uint8_t> buf = pack(current);
                client_send_unreliable(buf.data(), buf.size());
                s_last_sent_state = current;
            }
        }
    }
}

} // namespace kmp
