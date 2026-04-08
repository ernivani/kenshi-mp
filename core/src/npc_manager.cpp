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
#include <kenshi/Character.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Faction.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RaceData.h>
#include <kenshi/CharMovement.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreMath.h>
#include <OgreLogManager.h>
#include "kmp_log.h"

#include "packets.h"
#include "protocol.h"

namespace kmp {

extern GameWorld* game_get_world();
extern RootObjectFactory* game_get_factory();

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
        // Try neutral factions that exist in vanilla Kenshi
        const char* neutral_factions[] = {
            "drifters", "traders guild", "wanderer", "tech hunters",
            NULL
        };

        for (int i = 0; neutral_factions[i] != NULL; ++i) {
            Faction* f = ou->factionMgr->getFactionByName(std::string(neutral_factions[i]));
            if (f) {
                s_cached = f;
                KMP_LOG("[KenshiMP] Using neutral faction: " + std::string(neutral_factions[i]));
                return s_cached;
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

// Track hidden local NPCs (joiner mode)
struct HiddenNPC {
    Character* ch;
    float orig_x, orig_y, orig_z;
};
static std::vector<HiddenNPC> s_hidden_npcs;
static bool s_local_npcs_hidden = false;

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
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
    if (s_local_npcs_hidden) return;
    if (!ou) return;

    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
    ogre_unordered_set<Character*>::type::const_iterator it;
    for (it = chars.begin(); it != chars.end(); ++it) {
        Character* ch = *it;
        if (!ch) continue;
        if (ch->isPlayerCharacter()) continue;

        Ogre::Vector3 pos = ch->getPosition();
        HiddenNPC hidden;
        hidden.ch = ch;
        hidden.orig_x = pos.x;
        hidden.orig_y = pos.y;
        hidden.orig_z = pos.z;
        s_hidden_npcs.push_back(hidden);

        // Teleport far underground
        Ogre::Vector3 hide_pos(pos.x, -99999.0f, pos.z);
        Ogre::Quaternion rot(Ogre::Radian(0), Ogre::Vector3::UNIT_Y);
        ch->teleport(hide_pos, rot);
    }

    s_local_npcs_hidden = true;
    KMP_LOG(
        "[KenshiMP] Hidden " + itos(static_cast<uint32_t>(s_hidden_npcs.size())) + " local NPCs");
}

void npc_manager_show_local_npcs() {
    if (!s_local_npcs_hidden) return;

    for (size_t i = 0; i < s_hidden_npcs.size(); ++i) {
        HiddenNPC& h = s_hidden_npcs[i];
        if (h.ch) {
            Ogre::Vector3 pos(h.orig_x, h.orig_y, h.orig_z);
            Ogre::Quaternion rot(Ogre::Radian(0), Ogre::Vector3::UNIT_Y);
            h.ch->teleport(pos, rot);
        }
    }

    KMP_LOG(
        "[KenshiMP] Restored " + itos(static_cast<uint32_t>(s_hidden_npcs.size())) + " local NPCs");
    s_hidden_npcs.clear();
    s_local_npcs_hidden = false;
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

        if (dist_sq > 100.0f * 100.0f) {
            // Far away — teleport immediately
            Ogre::Quaternion rot(Ogre::Radian(entry.yaw), Ogre::Vector3::UNIT_Y);
            rnpc.npc->teleport(target, rot);
        } else {
            // Close enough — use setDestination for natural walking animation
            CharMovement* movement = rnpc.npc->getMovement();
            if (movement) {
                movement->setDestination(target, HIGH_PRIORITY, false);
            } else {
                // Fallback to teleport
                Ogre::Quaternion rot(Ogre::Radian(entry.yaw), Ogre::Vector3::UNIT_Y);
                rnpc.npc->teleport(target, rot);
            }
        }
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
    // Hide local NPCs on joiner (game keeps spawning new ones)
    if (s_local_npcs_hidden && ou) {
        // Build a set of our synced NPC pointers for fast lookup
        std::set<Character*> our_npcs;
        std::map<uint32_t, RemoteNPC>::iterator rn;
        for (rn = s_remote_npcs.begin(); rn != s_remote_npcs.end(); ++rn) {
            if (rn->second.npc) our_npcs.insert(rn->second.npc);
        }
        std::map<uint32_t, RemotePlayer>::iterator rp;
        for (rp = s_remote_players.begin(); rp != s_remote_players.end(); ++rp) {
            if (rp->second.npc) our_npcs.insert(rp->second.npc);
        }

        const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
        ogre_unordered_set<Character*>::type::const_iterator hide_it;
        for (hide_it = chars.begin(); hide_it != chars.end(); ++hide_it) {
            Character* ch = *hide_it;
            if (!ch) continue;
            if (ch->isPlayerCharacter()) continue;
            if (our_npcs.count(ch)) continue;  // don't hide our synced NPCs

            Ogre::Vector3 pos = ch->getPosition();
            if (pos.y > -90000.0f) {
                Ogre::Vector3 hide_pos(pos.x, -99999.0f, pos.z);
                Ogre::Quaternion rot(Ogre::Radian(0), Ogre::Vector3::UNIT_Y);
                ch->teleport(hide_pos, rot);
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
}

} // namespace kmp
