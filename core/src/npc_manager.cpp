// npc_manager.cpp — Manage NPCs representing remote players
//
// Each remote player is represented as a Kenshi NPC in the local game world.
// We spawn them via KenshiLib's factory, update their positions with
// interpolation, and despawn them on disconnect.

#include <map>
#include <cstring>

#include "packets.h"
#include "protocol.h"

namespace kmp {

// ---------------------------------------------------------------------------
// Remote player state (for interpolation)
// ---------------------------------------------------------------------------
struct RemotePlayer {
    uint32_t player_id;
    char     name[MAX_NAME_LENGTH];
    char     model[MAX_MODEL_LENGTH];

    // NPC handle — opaque pointer to KenshiLib Character object
    void*    npc_handle;

    // Interpolation: we keep two snapshots and blend between them
    struct Snapshot {
        float x, y, z;
        float yaw;
        uint32_t animation_id;
        float speed;
        double timestamp;   // local time when received
    };

    Snapshot prev;
    Snapshot next;
    double   interp_t;       // 0.0 = prev, 1.0 = next
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static std::map<uint32_t, RemotePlayer> s_remote_players;

// Monotonic clock for interpolation timing
static double get_time_sec() {
    // TODO: Use Ogre::Root::getSingleton().getTimer()->getMilliseconds() / 1000.0
    // or QueryPerformanceCounter on Windows
    return 0.0;
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void npc_manager_init() {
    s_remote_players.clear();
}

void npc_manager_shutdown() {
    // Despawn all NPCs
    for (auto& pair : s_remote_players) {
        if (pair.second.npc_handle) {
            // TODO: KenshiLib — destroy/despawn the NPC
            // factory->destroyObject(pair.second.npc_handle);
        }
    }
    s_remote_players.clear();
}

// ---------------------------------------------------------------------------
// Spawn a new remote player NPC
// ---------------------------------------------------------------------------
void npc_manager_on_spawn(const SpawnNPC& pkt) {
    // Don't spawn duplicates
    if (s_remote_players.count(pkt.player_id)) return;

    RemotePlayer rp;
    std::memset(&rp, 0, sizeof(rp));
    rp.player_id = pkt.player_id;
    std::strncpy(rp.name, pkt.name, MAX_NAME_LENGTH - 1);
    std::strncpy(rp.model, pkt.model, MAX_MODEL_LENGTH - 1);

    // Initialize position
    rp.prev = { pkt.x, pkt.y, pkt.z, pkt.yaw, 0, 0.0f, get_time_sec() };
    rp.next = rp.prev;
    rp.interp_t = 1.0;

    // TODO: Spawn NPC via KenshiLib
    //
    // Typical pattern using KenshiLib's RootObjectFactory:
    //   auto* factory = static_cast<RootObjectFactory*>(game_get_factory());
    //   if (!factory) return;
    //
    //   // Create a new character with the remote player's model/race
    //   auto* npc = factory->createCharacter(rp.model);
    //   npc->setPosition(Ogre::Vector3(pkt.x, pkt.y, pkt.z));
    //   npc->setName(rp.name);
    //
    //   // Disable AI so we control movement directly
    //   npc->setAI(false);
    //
    //   rp.npc_handle = npc;
    rp.npc_handle = nullptr;  // placeholder

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

    if (it->second.npc_handle) {
        // TODO: KenshiLib — destroy/despawn the NPC
        // factory->destroyObject(it->second.npc_handle);
    }

    s_remote_players.erase(it);
}

// ---------------------------------------------------------------------------
// Per-frame update — interpolate all remote player NPCs
// ---------------------------------------------------------------------------
static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Lerp angles handling wrap-around
static float lerp_angle(float a, float b, float t) {
    float diff = b - a;
    // Normalize to [-PI, PI]
    while (diff > 3.14159265f)  diff -= 6.28318530f;
    while (diff < -3.14159265f) diff += 6.28318530f;
    return a + diff * t;
}

void npc_manager_update(float dt) {
    for (auto& pair : s_remote_players) {
        RemotePlayer& rp = pair.second;

        // Advance interpolation
        // We interpolate over one tick interval (1/TICK_RATE_HZ seconds)
        if (rp.interp_t < 1.0f) {
            rp.interp_t += dt * TICK_RATE_HZ;
            if (rp.interp_t > 1.0f) rp.interp_t = 1.0f;
        }

        float t = static_cast<float>(rp.interp_t);
        float ix = lerp(rp.prev.x, rp.next.x, t);
        float iy = lerp(rp.prev.y, rp.next.y, t);
        float iz = lerp(rp.prev.z, rp.next.z, t);
        float iyaw = lerp_angle(rp.prev.yaw, rp.next.yaw, t);

        if (rp.npc_handle) {
            // TODO: Apply position to NPC via KenshiLib
            //   Character* npc = static_cast<Character*>(rp.npc_handle);
            //   npc->setPosition(Ogre::Vector3(ix, iy, iz));
            //   npc->setRotation(Ogre::Quaternion(Ogre::Radian(iyaw), Ogre::Vector3::UNIT_Y));
            //   npc->setAnimation(rp.next.animation_id);
            //   npc->setSpeed(rp.next.speed);
        }
    }
}

} // namespace kmp
