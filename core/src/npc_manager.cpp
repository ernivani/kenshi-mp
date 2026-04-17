// npc_manager.cpp — Manage NPCs representing remote players
//
// Runs on the game thread. Spawns/moves/despawns NPCs via KenshiLib.

#include <map>
#include <set>
#include <vector>
#include <cstring>
#include <string>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/GameData.h>
#include <kenshi/Character.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Faction.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RaceData.h>
#include <kenshi/CharMovement.h>
#include <kenshi/MedicalSystem.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreMath.h>
#include <OgreLogManager.h>
#include "kmp_log.h"

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

extern GameWorld* game_get_world();
extern RootObjectFactory* game_get_factory();

extern void client_send_reliable(const uint8_t* data, size_t length);
extern bool client_is_connected();
extern bool host_sync_is_host();
extern Character* game_get_player_character();

// Get faction for synced NPCs
// createRandomCharacter needs a faction with character templates
// Try known neutral factions first, fall back to player faction
static Faction* get_kmp_faction() {
    static Faction* s_cached = NULL;
    static bool s_tried = false;

    if (s_cached) return s_cached;
    if (s_tried) {
        // Already tried, use player faction
        if (ou && ou->player) return ou->player->getFaction();
        return NULL;
    }

    s_tried = true;

    if (ou && ou->factionMgr) {
        // Try neutral factions — search by both name and stringID
        const char* names[] = {
            "Drifters", "drifters", "Traders Guild", "traders guild",
            "Wanderer", "wanderer", "Tech Hunters", "tech hunters",
            "Neutral", "neutral", "Civilian", NULL
        };

        for (int i = 0; names[i] != NULL; ++i) {
            Faction* f = ou->factionMgr->getFactionByName(std::string(names[i]));
            if (f) {
                s_cached = f;
                KMP_LOG("[KenshiMP] Using neutral faction (by name): " + std::string(names[i]));
                return s_cached;
            }
            f = ou->factionMgr->getFactionByStringID(std::string(names[i]));
            if (f) {
                s_cached = f;
                KMP_LOG("[KenshiMP] Using neutral faction (by ID): " + std::string(names[i]));
                return s_cached;
            }
        }

        // Dump first 10 faction names for debugging
        const lektor<Faction*>* all = ou->factionMgr->getAllFactions();
        if (all) {
            int count = 0;
            for (int i = 0; i < all->count && count < 10; ++i) {
                Faction* f = all->stuff[i];
                if (f && f->data) {
                    KMP_LOG("[KenshiMP]   faction: '" + f->data->name + "'");
                    count++;
                }
            }
        }
    }

    // Fallback to player faction
    KMP_LOG("[KenshiMP] No neutral faction found, using player faction");
    if (ou && ou->player) return ou->player->getFaction();
    return NULL;
}

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
    float   last_health;
};

static std::map<uint32_t, RemotePlayer> s_remote_players;

// ---------------------------------------------------------------------------
// Remote NPC sync (from host via server)
// ---------------------------------------------------------------------------
struct RemoteNPC {
    uint32_t   npc_id;
    Character* npc;
    Snapshot   prev;
    Snapshot   next;
    double     interp_t;
};

static std::map<uint32_t, RemoteNPC> s_remote_npcs;

// Joiner-side NPC wipe: destroy local NPCs one per tick (same pattern as
// building_manager — batch>1 crashes due to cascade invalidation).
static bool  s_npc_wipe_active = false;
static float s_npc_wipe_timer  = 0.0f;
static const float NPC_WIPE_INTERVAL = 0.1f;  // 100ms between destroys

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
uint32_t npc_manager_get_nearest_remote_npc(float px, float py, float pz, float max_range) {
    float best_dist_sq = max_range * max_range;
    uint32_t best_id = 0;

    std::map<uint32_t, RemoteNPC>::iterator it;
    for (it = s_remote_npcs.begin(); it != s_remote_npcs.end(); ++it) {
        if (!it->second.npc) continue;
        Ogre::Vector3 pos = it->second.npc->getPosition();
        float dx = px - pos.x;
        float dy = py - pos.y;
        float dz = pz - pos.z;
        float d = dx*dx + dy*dy + dz*dz;
        if (d < best_dist_sq) {
            best_dist_sq = d;
            best_id = it->second.npc_id;
        }
    }
    return best_id;
}

Character* npc_manager_get_player_avatar(uint32_t player_id) {
    std::map<uint32_t, RemotePlayer>::iterator it = s_remote_players.find(player_id);
    if (it != s_remote_players.end()) return it->second.npc;
    return NULL;
}

bool npc_manager_is_player_npc(Character* ch) {
    std::map<uint32_t, RemotePlayer>::iterator it;
    for (it = s_remote_players.begin(); it != s_remote_players.end(); ++it) {
        if (it->second.npc == ch) return true;
    }
    return false;
}

void npc_manager_init() {
    s_remote_players.clear();
}

void npc_manager_hide_local_npcs() {
    if (s_npc_wipe_active) return;
    s_npc_wipe_active = true;
    s_npc_wipe_timer = 0.0f;
    KMP_LOG("[KenshiMP] NPC wipe active (scan-every-tick, 1 per "
        + itos(static_cast<uint32_t>(NPC_WIPE_INTERVAL * 1000)) + "ms)");
}

void npc_manager_show_local_npcs() {
    s_npc_wipe_active = false;
    KMP_LOG("[KenshiMP] NPC wipe deactivated (destroyed NPCs NOT restored — reload save)");
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

    std::map<uint32_t, RemoteNPC>::iterator rnpc_it;
    for (rnpc_it = s_remote_npcs.begin(); rnpc_it != s_remote_npcs.end(); ++rnpc_it) {
        if (rnpc_it->second.npc && world) {
            world->destroy(rnpc_it->second.npc, false, "KenshiMP shutdown");
        }
    }
    s_remote_npcs.clear();
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
    rp.last_health = -1.0f;

    // Skip spawning if position is at origin (player hasn't sent position yet)
    if (pkt.x == 0.0f && pkt.y == 0.0f && pkt.z == 0.0f) {
        KMP_LOG(
            "[KenshiMP] Deferred NPC spawn for player " + itos(pkt.player_id) + " (no position yet)");
        s_remote_players[pkt.player_id] = rp;
        return;
    }

    // Spawn NPC via KenshiLib (safe — we're on the game thread)
    RootObjectFactory* factory = game_get_factory();
    if (factory) {
        Ogre::Vector3 spawn_pos(pkt.x, pkt.y, pkt.z);

        Faction* faction = NULL;
        if (ou && ou->factionMgr) faction = ou->factionMgr->getEmptyFaction();
        if (!faction && ou && ou->player) faction = ou->player->getFaction();
        if (!faction) {
            KMP_LOG("[KenshiMP] WARNING: No faction available for NPC spawn");
            return;
        }
        RootObjectBase* obj = factory->createRandomCharacter(
            faction, spawn_pos, NULL, NULL, NULL, 0.0f
        );

        Character* npc = dynamic_cast<Character*>(obj);
        if (npc) {
            rp.npc = npc;
            KMP_LOG(
                "[KenshiMP] Spawned NPC for player " + itos(pkt.player_id));
        } else {
            KMP_LOG(
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

    // If NPC was deferred (no position at spawn time), spawn now at real position
    if (!rp.npc && pkt.x != 0.0f && pkt.y != 0.0f) {
        RootObjectFactory* factory = game_get_factory();
        if (factory) {
            Ogre::Vector3 spawn_pos(pkt.x, pkt.y, pkt.z);
            Faction* faction = NULL;
            if (ou && ou->factionMgr) faction = ou->factionMgr->getEmptyFaction();
            if (!faction && ou && ou->player) faction = ou->player->getFaction();
            if (faction) {
                RootObjectBase* obj = factory->createRandomCharacter(
                    faction, spawn_pos, NULL, NULL, NULL, 0.0f
                );
                Character* npc = dynamic_cast<Character*>(obj);
                if (npc) {
                    rp.npc = npc;
                    KMP_LOG(
                        "[KenshiMP] Late-spawned NPC for player " + itos(pkt.player_id));
                }
            }
        }
    }

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
        KMP_LOG(
            "[KenshiMP] Despawned NPC for player " + itos(player_id));
    }

    s_remote_players.erase(it);
}

// ---------------------------------------------------------------------------
// Remote NPC handlers (from host sync)
// ---------------------------------------------------------------------------
void npc_manager_on_remote_spawn(const NPCSpawnRemote& pkt) {
    if (s_remote_npcs.count(pkt.npc_id)) return;

    RemoteNPC rnpc;
    std::memset(&rnpc, 0, sizeof(rnpc));
    rnpc.npc_id = pkt.npc_id;
    rnpc.npc = NULL;

    double now = get_time_sec();
    Snapshot snap;
    snap.x = pkt.x;
    snap.y = pkt.y;
    snap.z = pkt.z;
    snap.yaw = pkt.yaw;
    snap.animation_id = 0;
    snap.speed = 0.0f;
    snap.timestamp = now;
    rnpc.prev = snap;
    rnpc.next = snap;
    rnpc.interp_t = 1.0;

    RootObjectFactory* factory = game_get_factory();
    if (factory) {
        Ogre::Vector3 spawn_pos(pkt.x, pkt.y, pkt.z);

        Faction* faction = get_kmp_faction();
        if (!faction) {
            KMP_LOG("[KenshiMP] WARNING: No faction available for NPC spawn");
            return;
        }

        RootObjectBase* obj = factory->createRandomCharacter(
            faction, spawn_pos, NULL, NULL, NULL, 0.0f
        );

        Character* npc = dynamic_cast<Character*>(obj);
        if (npc) {
            rnpc.npc = npc;
            KMP_LOG(
                "[KenshiMP] Spawned remote NPC " + itos(pkt.npc_id) +
                " '" + std::string(pkt.name) + "' race=" + std::string(pkt.race));
        } else {
            KMP_LOG("[KenshiMP] WARNING: createRandomCharacter failed for NPC " + itos(pkt.npc_id));
        }
    }

    s_remote_npcs[pkt.npc_id] = rnpc;
}

void npc_manager_on_remote_state(const NPCStateEntry& entry) {
    std::map<uint32_t, RemoteNPC>::iterator it = s_remote_npcs.find(entry.npc_id);
    if (it == s_remote_npcs.end()) return;

    RemoteNPC& rnpc = it->second;
    rnpc.prev = rnpc.next;

    Snapshot snap;
    snap.x = entry.x;
    snap.y = entry.y;
    snap.z = entry.z;
    snap.yaw = entry.yaw;
    snap.animation_id = entry.animation_id;
    snap.speed = entry.speed;
    snap.timestamp = get_time_sec();
    rnpc.next = snap;
    rnpc.interp_t = 0.0;

    // Move NPC to target position
    if (rnpc.npc) {
        Ogre::Vector3 target(entry.x, entry.y, entry.z);
        Ogre::Vector3 current = rnpc.npc->getPosition();

        float dx = target.x - current.x;
        float dy = target.y - current.y;
        float dz = target.z - current.z;
        float dist_sq = dx*dx + dy*dy + dz*dz;

        // Always teleport — setDestination gets overridden by NPC AI
        Ogre::Quaternion rot(Ogre::Radian(entry.yaw), Ogre::Vector3::UNIT_Y);
        rnpc.npc->teleport(target, rot);
    }
}

void npc_manager_on_remote_despawn(uint32_t npc_id) {
    std::map<uint32_t, RemoteNPC>::iterator it = s_remote_npcs.find(npc_id);
    if (it == s_remote_npcs.end()) return;

    if (it->second.npc) {
        GameWorld* world = game_get_world();
        if (world) {
            world->destroy(it->second.npc, false, "KenshiMP NPC despawn");
        }
        KMP_LOG(
            "[KenshiMP] Despawned remote NPC " + itos(npc_id));
    }

    s_remote_npcs.erase(it);
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
    // Destroy local NPCs on joiner — one per tick, closest first
    if (s_npc_wipe_active && ou) {
        s_npc_wipe_timer += dt;
        if (s_npc_wipe_timer >= NPC_WIPE_INTERVAL) {
            s_npc_wipe_timer = 0.0f;

            // Build set of our synced NPC pointers to skip
            std::set<Character*> our_npcs;
            std::map<uint32_t, RemoteNPC>::iterator rn;
            for (rn = s_remote_npcs.begin(); rn != s_remote_npcs.end(); ++rn) {
                if (rn->second.npc) our_npcs.insert(rn->second.npc);
            }
            std::map<uint32_t, RemotePlayer>::iterator rp;
            for (rp = s_remote_players.begin(); rp != s_remote_players.end(); ++rp) {
                if (rp->second.npc) our_npcs.insert(rp->second.npc);
            }

            // Find closest non-player, non-synced NPC
            Character* player_ch = game_get_player_character();
            Ogre::Vector3 center(0, 0, 0);
            if (player_ch) center = player_ch->getPosition();

            Character* closest = NULL;
            float closest_dist2 = 1e30f;

            const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
            ogre_unordered_set<Character*>::type::const_iterator cit;
            for (cit = chars.begin(); cit != chars.end(); ++cit) {
                Character* ch = *cit;
                if (!ch) continue;
                if (ch->isPlayerCharacter()) continue;
                if (our_npcs.count(ch)) continue;

                if (player_ch) {
                    float d2 = center.squaredDistance(ch->getPosition());
                    if (d2 < closest_dist2) {
                        closest = ch;
                        closest_dist2 = d2;
                    }
                } else {
                    closest = ch;
                    break;
                }
            }

            if (closest) {
                KMP_LOG("[KenshiMP] DESTROY NPC ptr="
                    + itos(static_cast<uint32_t>((uint64_t)(uintptr_t)closest & 0xFFFFFFFF)));
                ou->destroy(closest, false, "KenshiMP");
                KMP_LOG("[KenshiMP] DESTROY NPC ok");
            }
        }
    }

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
            Ogre::Vector3 target(ix, iy, iz);
            Ogre::Vector3 current = rp.npc->getPosition();
            float dx = target.x - current.x;
            float dz = target.z - current.z;
            float dist_sq = dx*dx + dz*dz;

            if (dist_sq > 50.0f * 50.0f) {
                Ogre::Quaternion rot(Ogre::Radian(iyaw), Ogre::Vector3::UNIT_Y);
                rp.npc->teleport(target, rot);
            } else {
                CharMovement* movement = rp.npc->getMovement();
                if (movement) {
                    movement->setDestination(target, HIGH_PRIORITY, false);
                }
            }
        }
    }

    // Remote NPCs use setDestination from on_remote_state, no interpolation loop needed

    // Combat: host applies stats and initiates combat for joiner avatars
    // (handled via packets in player_sync.cpp → host_sync.cpp)
}

} // namespace kmp
