// world_state.cpp — Server-side authoritative world state
//
// Tracks all player positions and state for zone filtering and persistence.

#include <map>
#include <cstring>

#include <spdlog/spdlog.h>

#include "protocol.h"

namespace kmp {

// ---------------------------------------------------------------------------
// Player world state
// ---------------------------------------------------------------------------
struct WorldPlayer {
    uint32_t id;
    char     name[MAX_NAME_LENGTH];
    char     model[MAX_MODEL_LENGTH];
    float    x, y, z;
    float    yaw;
    uint32_t animation_id;
    float    speed;
};

static std::map<uint32_t, WorldPlayer> s_players;

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void world_state_init() {
    s_players.clear();
    spdlog::info("World state initialized");
}

void world_state_add_player(uint32_t id, const char* name, const char* model) {
    WorldPlayer wp;
    std::memset(&wp, 0, sizeof(wp));
    wp.id = id;
    std::strncpy(wp.name, name, MAX_NAME_LENGTH - 1);
    std::strncpy(wp.model, model, MAX_MODEL_LENGTH - 1);
    s_players[id] = wp;
}

void world_state_remove_player(uint32_t id) {
    s_players.erase(id);
}

void world_state_update_position(uint32_t id, float x, float y, float z, float yaw,
                                  uint32_t anim, float speed) {
    auto it = s_players.find(id);
    if (it == s_players.end()) return;

    it->second.x = x;
    it->second.y = y;
    it->second.z = z;
    it->second.yaw = yaw;
    it->second.animation_id = anim;
    it->second.speed = speed;
}

// Check if two players are within view distance of each other
bool world_state_in_range(uint32_t id_a, uint32_t id_b, float max_distance) {
    auto a = s_players.find(id_a);
    auto b = s_players.find(id_b);
    if (a == s_players.end() || b == s_players.end()) return false;

    float dx = a->second.x - b->second.x;
    float dy = a->second.y - b->second.y;
    float dz = a->second.z - b->second.z;
    return (dx*dx + dy*dy + dz*dz) <= (max_distance * max_distance);
}

uint32_t world_state_player_count() {
    return static_cast<uint32_t>(s_players.size());
}

} // namespace kmp
