#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kmp {

struct SpawnedNPC {
    uint32_t id;
    std::string name;
    std::string race;
    std::string weapon;
    std::string armour;
    float x, y, z, yaw;
    bool  enable_ai;
};

struct SpawnedBuilding {
    uint32_t id;
    std::string stringID;
    float x, y, z;
    float qw, qx, qy, qz;
    bool  completed;
    bool  is_foliage;
    int16_t floor;
};

struct NPCSpawnRequest {
    std::string name;
    std::string race;
    std::string weapon;
    std::string armour;
    float x, y, z, yaw;
    bool  enable_ai;   // default false — NPC stands still
};

struct BuildingSpawnRequest {
    std::string stringID;
    float x, y, z;
    float qw, qx, qy, qz;
    bool  completed;
    bool  is_foliage;
    int16_t floor;
};

// Allocate a server-owned id, broadcast SERVER_SPAWN_NPC/BUILDING to all peers,
// and remember the entity so it can be listed / despawned from the GUI later.
uint32_t spawn_npc(const NPCSpawnRequest& req);
uint32_t spawn_building(const BuildingSpawnRequest& req);

bool     despawn_npc(uint32_t id);
bool     despawn_building(uint32_t id);

void     spawned_npcs(std::vector<SpawnedNPC>& out);
void     spawned_buildings(std::vector<SpawnedBuilding>& out);

} // namespace kmp
