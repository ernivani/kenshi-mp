#include "spawn.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

#include <enet/enet.h>
#include <spdlog/spdlog.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

void relay_broadcast(ENetPeer* exclude, const uint8_t* data, size_t length, bool reliable);

// Server-owned IDs live in 0x7F000000+. This keeps them far from host-allocated
// IDs (host typically counts up from small numbers) so there's no collision
// even if the host keeps running while the server spawns things.
static std::atomic<uint32_t> s_next_id{0x7F000000u};
static std::mutex s_mu;
static std::map<uint32_t, SpawnedNPC>      s_npcs;
static std::map<uint32_t, SpawnedBuilding> s_buildings;

uint32_t spawn_npc(const NPCSpawnRequest& req) {
    uint32_t id = s_next_id.fetch_add(1, std::memory_order_relaxed);

    NPCSpawnRemote pkt;
    pkt.header.type = PacketType::SERVER_SPAWN_NPC;
    pkt.npc_id = id;
    safe_strcpy(pkt.name,   req.name.c_str());
    safe_strcpy(pkt.race,   req.race.c_str());
    safe_strcpy(pkt.weapon, req.weapon.c_str());
    safe_strcpy(pkt.armour, req.armour.c_str());
    pkt.x = req.x; pkt.y = req.y; pkt.z = req.z; pkt.yaw = req.yaw;

    auto buf = pack(pkt);
    relay_broadcast(nullptr, buf.data(), buf.size(), true);

    SpawnedNPC s;
    s.id = id;
    s.name = req.name; s.race = req.race;
    s.weapon = req.weapon; s.armour = req.armour;
    s.x = req.x; s.y = req.y; s.z = req.z; s.yaw = req.yaw;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        s_npcs[id] = s;
    }
    spdlog::info("Server-spawned NPC {} '{}' at ({:.1f}, {:.1f}, {:.1f})",
        id, req.name, req.x, req.y, req.z);
    return id;
}

uint32_t spawn_building(const BuildingSpawnRequest& req) {
    uint32_t id = s_next_id.fetch_add(1, std::memory_order_relaxed);

    BuildingSpawnRemote pkt;
    pkt.header.type = PacketType::SERVER_SPAWN_BUILDING;
    pkt.building_id = id;
    safe_strcpy(pkt.stringID, req.stringID.c_str());
    pkt.x = req.x; pkt.y = req.y; pkt.z = req.z;
    pkt.qw = req.qw; pkt.qx = req.qx; pkt.qy = req.qy; pkt.qz = req.qz;
    pkt.completed = req.completed ? 1 : 0;
    pkt.is_foliage = req.is_foliage ? 1 : 0;
    pkt.floor = req.floor;

    auto buf = pack(pkt);
    relay_broadcast(nullptr, buf.data(), buf.size(), true);

    SpawnedBuilding s;
    s.id = id;
    s.stringID = req.stringID;
    s.x = req.x; s.y = req.y; s.z = req.z;
    s.qw = req.qw; s.qx = req.qx; s.qy = req.qy; s.qz = req.qz;
    s.completed  = req.completed;
    s.is_foliage = req.is_foliage;
    s.floor = req.floor;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        s_buildings[id] = s;
    }
    spdlog::info("Server-spawned Building {} '{}' at ({:.1f}, {:.1f}, {:.1f})",
        id, req.stringID, req.x, req.y, req.z);
    return id;
}

bool despawn_npc(uint32_t id) {
    {
        std::lock_guard<std::mutex> lk(s_mu);
        if (s_npcs.erase(id) == 0) return false;
    }
    NPCDespawnRemote pkt;
    pkt.header.type = PacketType::SERVER_DESPAWN_NPC;
    pkt.npc_id = id;
    auto buf = pack(pkt);
    relay_broadcast(nullptr, buf.data(), buf.size(), true);
    spdlog::info("Server-despawned NPC {}", id);
    return true;
}

bool despawn_building(uint32_t id) {
    {
        std::lock_guard<std::mutex> lk(s_mu);
        if (s_buildings.erase(id) == 0) return false;
    }
    BuildingDespawnRemote pkt;
    pkt.header.type = PacketType::SERVER_DESPAWN_BUILDING;
    pkt.building_id = id;
    auto buf = pack(pkt);
    relay_broadcast(nullptr, buf.data(), buf.size(), true);
    spdlog::info("Server-despawned Building {}", id);
    return true;
}

void spawned_npcs(std::vector<SpawnedNPC>& out) {
    std::lock_guard<std::mutex> lk(s_mu);
    out.clear();
    out.reserve(s_npcs.size());
    for (auto& p : s_npcs) out.push_back(p.second);
}

void spawned_buildings(std::vector<SpawnedBuilding>& out) {
    std::lock_guard<std::mutex> lk(s_mu);
    out.clear();
    out.reserve(s_buildings.size());
    for (auto& p : s_buildings) out.push_back(p.second);
}

} // namespace kmp
