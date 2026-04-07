// npc_manager.cpp — Manage NPCs representing remote players
//
// Runs on the game thread. Spawns/moves/despawns NPCs via KenshiLib.

#include <map>
#include <cstring>
#include <string>
#include <sstream>

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

extern GameWorld* game_get_world();
extern RootObjectFactory* game_get_factory();

// ---------------------------------------------------------------------------
// High-resolution monotonic clock
// ---------------------------------------------------------------------------
static double get_time_sec() {
    static LARGE_INTEGER freq;
    static bool freq_init = false;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

// v100-safe int to string
static std::string itos(uint32_t val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Remote player state
// ---------------------------------------------------------------------------
struct Snapshot {
    float x, y, z;
    float yaw;
    uint32_t animation_id;
    float speed;
    double timestamp;
};

struct RemotePlayer {
    uint32_t player_id;
    char     name[MAX_NAME_LENGTH];
    char     model[MAX_MODEL_LENGTH];
    Character* npc;
    Snapshot prev;
    Snapshot next;
    double  interp_t;
};

static std::map<uint32_t, RemotePlayer> s_remote_players;

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void npc_manager_init() {
    s_remote_players.clear();
}

void npc_manager_shutdown() {
    GameWorld* world = game_get_world();
    std::map<uint32_t, RemotePlayer>::iterator it;
    for (it = s_remote_players.begin(); it != s_remote_players.end(); ++it) {
        if (it->second.npc && world) {
            world->destroy(it->second.npc, false, "KenshiMP shutdown");
        }
    }
    s_remote_players.clear();
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void npc_manager_on_spawn(const SpawnNPC& pkt) {
    if (s_remote_players.count(pkt.player_id)) return;

    RemotePlayer rp;
    std::memset(&rp, 0, sizeof(rp));
    rp.player_id = pkt.player_id;
    std::strncpy(rp.name, pkt.name, MAX_NAME_LENGTH - 1);
    std::strncpy(rp.model, pkt.model, MAX_MODEL_LENGTH - 1);
    rp.npc = NULL;

    double now = get_time_sec();
    Snapshot snap;
    snap.x = pkt.x;
    snap.y = pkt.y;
    snap.z = pkt.z;
    snap.yaw = pkt.yaw;
    snap.animation_id = 0;
    snap.speed = 0.0f;
    snap.timestamp = now;
    rp.prev = snap;
    rp.next = snap;
    rp.interp_t = 1.0;

    // Spawn NPC via KenshiLib (safe — we're on the game thread)
    RootObjectFactory* factory = game_get_factory();
    if (factory) {
        Ogre::Vector3 spawn_pos(pkt.x, pkt.y, pkt.z);

        RootObjectBase* obj = factory->createRandomCharacter(
            NULL, spawn_pos, NULL, NULL, NULL, 0.0f
        );

        Character* npc = dynamic_cast<Character*>(obj);
        if (npc) {
            if (npc->ai) {
                npc->ai = NULL;
            }
            rp.npc = npc;
            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] Spawned NPC for player " + itos(pkt.player_id));
        } else {
            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] WARNING: Failed to spawn NPC for player " + itos(pkt.player_id));
        }
    }

    s_remote_players[pkt.player_id] = rp;
}

// ---------------------------------------------------------------------------
// Update position from network
// ---------------------------------------------------------------------------
void npc_manager_on_state(const PlayerState& pkt) {
    std::map<uint32_t, RemotePlayer>::iterator it = s_remote_players.find(pkt.player_id);
    if (it == s_remote_players.end()) return;

    RemotePlayer& rp = it->second;
    rp.prev = rp.next;

    Snapshot snap;
    snap.x = pkt.x;
    snap.y = pkt.y;
    snap.z = pkt.z;
    snap.yaw = pkt.yaw;
    snap.animation_id = pkt.animation_id;
    snap.speed = pkt.speed;
    snap.timestamp = get_time_sec();
    rp.next = snap;
    rp.interp_t = 0.0;
}

// ---------------------------------------------------------------------------
// Disconnect
// ---------------------------------------------------------------------------
void npc_manager_on_disconnect(uint32_t player_id) {
    std::map<uint32_t, RemotePlayer>::iterator it = s_remote_players.find(player_id);
    if (it == s_remote_players.end()) return;

    if (it->second.npc) {
        GameWorld* world = game_get_world();
        if (world) {
            world->destroy(it->second.npc, false, "KenshiMP disconnect");
        }
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] Despawned NPC for player " + itos(player_id));
    }

    s_remote_players.erase(it);
}

// ---------------------------------------------------------------------------
// Per-frame interpolation + teleport
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
    std::map<uint32_t, RemotePlayer>::iterator it;
    for (it = s_remote_players.begin(); it != s_remote_players.end(); ++it) {
        RemotePlayer& rp = it->second;

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
