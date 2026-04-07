// npc_manager.cpp — Manage NPCs representing remote players
//
// Each remote player is represented as a Kenshi NPC in the local game world.
// Spawned via KenshiLib's RootObjectFactory, moved via Character::teleport(),
// despawned via GameWorld::destroy().

#include <map>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/RootObjectFactory.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreMath.h>
#include <OgreLogManager.h>

#include "packets.h"
#include "protocol.h"

namespace kmp {

// External
extern GameWorld* game_get_world();
extern RootObjectFactory* game_get_factory();

// ---------------------------------------------------------------------------
// High-resolution monotonic clock
// ---------------------------------------------------------------------------
static double get_time_sec() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

// ---------------------------------------------------------------------------
// Remote player state (for interpolation)
// ---------------------------------------------------------------------------
struct RemotePlayer {
    uint32_t player_id;
    char     name[MAX_NAME_LENGTH];
    char     model[MAX_MODEL_LENGTH];

    // NPC handle — KenshiLib Character object
    Character* npc;

    // Interpolation: two snapshots, blend between them
    struct Snapshot {
        float x, y, z;
        float yaw;
        uint32_t animation_id;
        float speed;
        double timestamp;
    };

    Snapshot prev;
    Snapshot next;
    double  interp_t;
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static std::map<uint32_t, RemotePlayer> s_remote_players;

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void npc_manager_init() {
    s_remote_players.clear();
}

void npc_manager_shutdown() {
    GameWorld* world = game_get_world();
    for (auto& pair : s_remote_players) {
        if (pair.second.npc && world) {
            world->destroy(pair.second.npc, false, "KenshiMP shutdown");
        }
    }
    s_remote_players.clear();
}

// ---------------------------------------------------------------------------
// Spawn a new remote player NPC
// ---------------------------------------------------------------------------
void npc_manager_on_spawn(const SpawnNPC& pkt) {
    if (s_remote_players.count(pkt.player_id)) return;

    RemotePlayer rp;
    std::memset(&rp, 0, sizeof(rp));
    rp.player_id = pkt.player_id;
    std::strncpy(rp.name, pkt.name, MAX_NAME_LENGTH - 1);
    std::strncpy(rp.model, pkt.model, MAX_MODEL_LENGTH - 1);
    rp.npc = nullptr;

    double now = get_time_sec();
    rp.prev = { pkt.x, pkt.y, pkt.z, pkt.yaw, 0, 0.0f, now };
    rp.next = rp.prev;
    rp.interp_t = 1.0;

    // Spawn NPC via KenshiLib
    RootObjectFactory* factory = game_get_factory();
    if (factory) {
        Ogre::Vector3 spawn_pos(pkt.x, pkt.y, pkt.z);
        Ogre::Quaternion spawn_rot(Ogre::Radian(pkt.yaw), Ogre::Vector3::UNIT_Y);

        // Use createRandomCharacter to spawn a basic humanoid NPC
        // faction = nullptr (neutral), no home building, age 0
        RootObjectBase* obj = factory->createRandomCharacter(
            nullptr,          // faction (neutral)
            spawn_pos,        // position
            nullptr,          // container
            nullptr,          // character template (default)
            nullptr,          // home building
            0.0f              // age
        );

        Character* npc = dynamic_cast<Character*>(obj);
        if (npc) {
            // Disable AI so we control movement directly via teleport
            if (npc->ai) {
                npc->ai = nullptr;
            }
            rp.npc = npc;

            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] Spawned NPC for player " + std::to_string(pkt.player_id)
                + " '" + std::string(pkt.name) + "'"
            );
        } else {
            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] WARNING: Failed to spawn NPC for player "
                + std::to_string(pkt.player_id)
            );
        }
    }

    s_remote_players[pkt.player_id] = rp;
}

// ---------------------------------------------------------------------------
// Update position from network
// ---------------------------------------------------------------------------
void npc_manager_on_state(const PlayerState& pkt) {
    auto it = s_remote_players.find(pkt.player_id);
    if (it == s_remote_players.end()) return;

    RemotePlayer& rp = it->second;

    // Shift: current "next" becomes "prev", new data becomes "next"
    rp.prev = rp.next;
    rp.next = {
        pkt.x, pkt.y, pkt.z,
        pkt.yaw,
        pkt.animation_id,
        pkt.speed,
        get_time_sec()
    };
    rp.interp_t = 0.0;
}

// ---------------------------------------------------------------------------
// Remove a disconnected player's NPC
// ---------------------------------------------------------------------------
void npc_manager_on_disconnect(uint32_t player_id) {
    auto it = s_remote_players.find(player_id);
    if (it == s_remote_players.end()) return;

    if (it->second.npc) {
        GameWorld* world = game_get_world();
        if (world) {
            world->destroy(it->second.npc, false, "KenshiMP disconnect");
        }

        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] Despawned NPC for player " + std::to_string(player_id)
        );
    }

    s_remote_players.erase(it);
}

// ---------------------------------------------------------------------------
// Per-frame update — interpolate all remote player NPCs
// ---------------------------------------------------------------------------
static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static float lerp_angle(float a, float b, float t) {
    float diff = b - a;
    while (diff > 3.14159265f)  diff -= 6.28318530f;
    while (diff < -3.14159265f) diff += 6.28318530f;
    return a + diff * t;
}

void npc_manager_update(float dt) {
    for (auto& pair : s_remote_players) {
        RemotePlayer& rp = pair.second;

        // Advance interpolation over one tick interval
        if (rp.interp_t < 1.0) {
            rp.interp_t += static_cast<double>(dt) * TICK_RATE_HZ;
            if (rp.interp_t > 1.0) rp.interp_t = 1.0;
        }

        float t = static_cast<float>(rp.interp_t);
        float ix = lerp(rp.prev.x, rp.next.x, t);
        float iy = lerp(rp.prev.y, rp.next.y, t);
        float iz = lerp(rp.prev.z, rp.next.z, t);
        float iyaw = lerp_angle(rp.prev.yaw, rp.next.yaw, t);

        if (rp.npc) {
            Ogre::Vector3 pos(ix, iy, iz);
            Ogre::Quaternion rot(Ogre::Radian(iyaw), Ogre::Vector3::UNIT_Y);
            rp.npc->teleport(pos, rot);
        }
    }
}

} // namespace kmp
