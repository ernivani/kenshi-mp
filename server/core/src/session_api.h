#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct _ENetPeer;
typedef struct _ENetPeer ENetPeer;

namespace kmp {

struct PlayerInfo {
    uint32_t    id;
    std::string name;
    std::string model;
    std::string address;
    uint32_t    ping_ms;
    bool        is_host;
    uint32_t    idle_ms;
    float       x, y, z;
    float       yaw;
    float       speed;
    uint32_t    last_animation_id;
    uint8_t     last_posture_flags;
};

void         session_get_players(std::vector<PlayerInfo>& out);
ENetPeer*    session_find_peer(uint32_t player_id);
bool         session_get_player_snapshot(uint32_t player_id, PlayerInfo& out);

// Chat ring (for GUI Chat pane). Server sentinel: player_id == 0.
struct ChatLogEntry {
    uint32_t    player_id;      // 0 = server
    std::string author;         // resolved name at log time ("<server>" if id=0)
    std::string text;
    uint64_t    time_ms;
};
void         session_chat_push(uint32_t player_id, const std::string& author, const std::string& text);
void         session_chat_snapshot(std::vector<ChatLogEntry>& out);

// Suppress the next "<name> left" announce for this player id. Used by
// admin_kick so it can broadcast its own "<name> was kicked" line instead.
void         session_suppress_leave_announce(uint32_t player_id);

// Posture transition log (for GUI Posture pane).
struct PostureTransition {
    uint32_t    player_id;
    std::string player_name;
    uint8_t     old_flags;
    uint8_t     new_flags;
    uint64_t    time_ms;
};
void         session_posture_snapshot(std::vector<PostureTransition>& out);

} // namespace kmp
