#include "admin.h"

#include <chrono>
#include <cstring>

#include <enet/enet.h>
#include <spdlog/spdlog.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"
#include "session_api.h"
#include "events.h"

namespace kmp {

// session.cpp / relay.cpp externs
void session_on_disconnect(ENetPeer* peer);
void relay_broadcast(ENetPeer* exclude, const uint8_t* data, size_t length, bool reliable);

static volatile bool* s_running_flag = nullptr;

struct StickyPosture {
    bool      active = false;
    uint32_t  target_id = 0;
    uint8_t   flags = 0;
    std::chrono::steady_clock::time_point last_send;
};
static StickyPosture s_sticky;

void admin_set_running_flag(volatile bool* running) {
    s_running_flag = running;
}

void admin_request_shutdown() {
    spdlog::info("Shutdown requested from GUI");
    if (s_running_flag) *s_running_flag = false;
}

void admin_kick(uint32_t player_id, const char* reason) {
    ENetPeer* peer = session_find_peer(player_id);
    if (!peer) {
        spdlog::warn("Kick: player {} not found", player_id);
        return;
    }
    spdlog::info("Kicking player {} (reason: {})", player_id, reason ? reason : "");
    // Send a server chat as a visible reason, best-effort.
    if (reason && reason[0]) {
        ChatMessage m;
        m.player_id = 0;
        safe_strcpy(m.message, (std::string("Kicked: ") + reason).c_str());
        auto buf = pack(m);
        ENetPacket* pkt = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, CHANNEL_RELIABLE, pkt);
        enet_host_flush(peer->host);
    }
    // Non-zero disconnect code tells the client this was an admin-initiated
    // kick (vs. a network drop) so it suppresses auto-reconnect.
    enet_peer_disconnect(peer, /*data=*/1);
    enet_host_flush(peer->host);
    session_on_disconnect(peer);
}

void admin_broadcast_chat(const std::string& text) {
    if (text.empty()) return;
    ChatMessage m;
    m.player_id = 0;
    safe_strcpy(m.message, text.c_str());
    auto buf = pack(m);
    relay_broadcast(nullptr, buf.data(), buf.size(), true);
    session_chat_push(0, "<server>", text);
    events_emit_chat(0, "<server>", text);
    spdlog::info("[Server chat] {}", text);
}

static void send_posture_for(const PlayerInfo& p, uint8_t flags) {
    PlayerState s;
    s.player_id = p.id;
    s.x = p.x;
    s.y = p.y;
    s.z = p.z;
    s.yaw = p.yaw;
    s.speed = p.speed;
    // Preserve any non-posture bits in the cached animation id.
    uint32_t anim = (p.last_animation_id & ~0xFFu) | flags;
    s.animation_id = anim;
    auto buf = pack(s);
    relay_broadcast(nullptr, buf.data(), buf.size(), false);
}

void admin_inject_posture(uint32_t target_player_id, uint8_t posture_flags, bool sticky) {
    PlayerInfo p;
    if (!session_get_player_snapshot(target_player_id, p)) {
        spdlog::warn("Posture inject: player {} not found", target_player_id);
        return;
    }
    spdlog::info("Posture inject: player {} flags=0x{:02x} sticky={}",
        target_player_id, posture_flags, sticky);
    send_posture_for(p, posture_flags);
    if (sticky) {
        s_sticky.active = true;
        s_sticky.target_id = target_player_id;
        s_sticky.flags = posture_flags;
        s_sticky.last_send = std::chrono::steady_clock::now();
    } else {
        s_sticky.active = false;
    }
}

void admin_clear_sticky_posture() {
    if (!s_sticky.active) return;
    spdlog::info("Posture sticky: clearing for player {}", s_sticky.target_id);
    PlayerInfo p;
    if (session_get_player_snapshot(s_sticky.target_id, p)) {
        send_posture_for(p, 0);
    }
    s_sticky.active = false;
}

bool admin_sticky_active() { return s_sticky.active; }
uint32_t admin_sticky_target() { return s_sticky.target_id; }
uint8_t admin_sticky_flags() { return s_sticky.flags; }

void admin_tick() {
    if (!s_sticky.active) return;
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - s_sticky.last_send).count();
    if (ms < 500) return;

    PlayerInfo p;
    if (!session_get_player_snapshot(s_sticky.target_id, p)) {
        // Target gone — drop sticky silently.
        s_sticky.active = false;
        return;
    }
    send_posture_for(p, s_sticky.flags);
    s_sticky.last_send = now;
}

} // namespace kmp
