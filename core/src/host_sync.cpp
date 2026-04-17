// host_sync.cpp — Host-side NPC state scanning and sync
//
// Runs on the game thread. Scans KenshiLib's character update list,
// filters by proximity to connected players, sends NPC spawn/state/despawn
// packets to the server for relay to joiners.

#include <map>
#include <vector>
#include <cstring>
#include <string>
#include <sstream>

#include <kenshi/Damages.h>
#include <kenshi/Enums.h>
#include <kenshi/GameData.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Faction.h>
#include <kenshi/RaceData.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/CharStats.h>
#include <OgreVector3.h>
#include <OgreLogManager.h>
#include "kmp_log.h"

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

extern void client_send_unreliable(const uint8_t* data, size_t length);
extern void client_send_reliable(const uint8_t* data, size_t length);
extern Character* game_get_player_character();
extern bool npc_manager_is_player_npc(Character* ch);
extern Character* npc_manager_get_player_avatar(uint32_t player_id);

// v100-safe int to string
static std::string itos(uint32_t val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

// ---------------------------------------------------------------------------
// NPC ID system: map Character pointer → compact sequential uint32_t
// ---------------------------------------------------------------------------
struct SyncedNPC {
    uint32_t npc_id;
    float    last_x, last_y, last_z;
    bool     is_test;  // test NPCs don't get auto-despawned
};

static std::map<uint64_t, SyncedNPC> s_synced_npcs;
static uint32_t s_next_npc_id = 1;
static bool     s_is_host = false;
static float    s_npc_send_timer = 0.0f;

static uint64_t make_npc_key(Character* ch) {
    return (uint64_t)(uintptr_t)ch;
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
static void fill_spawn_packet(NPCSpawnRemote& pkt, Character* ch, uint32_t npc_id);

void host_sync_init() {
    s_synced_npcs.clear();
    s_next_npc_id = 1;
    s_npc_send_timer = 0.0f;
}

// Resend all synced NPCs — called when a new joiner connects
void host_sync_resend_all() {
    if (!s_is_host) return;
    if (!ou) return;

    int count = 0;
    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();

    std::map<uint64_t, SyncedNPC>::iterator it;
    for (it = s_synced_npcs.begin(); it != s_synced_npcs.end(); ++it) {
        // Find the Character* from the key
        Character* ch = (Character*)(uintptr_t)it->first;

        // Verify it's still valid by checking if it's in the update list
        bool valid = false;
        ogre_unordered_set<Character*>::type::const_iterator cit;
        for (cit = chars.begin(); cit != chars.end(); ++cit) {
            if (*cit == ch) { valid = true; break; }
        }

        if (valid && ch) {
            NPCSpawnRemote spawn;
            fill_spawn_packet(spawn, ch, it->second.npc_id);
            std::vector<uint8_t> buf = pack(spawn);
            client_send_reliable(buf.data(), buf.size());
            count++;
        }
    }

    KMP_LOG("[KenshiMP] Resent " + itos(count) + " synced NPCs to new joiner");
}

void host_sync_on_combat_attack(const CombatAttack& pkt) {
    if (!s_is_host) return;
    if (!ou) return;

    // Find the real NPC by npc_id
    Character* target = NULL;
    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
    std::map<uint64_t, SyncedNPC>::iterator it;
    for (it = s_synced_npcs.begin(); it != s_synced_npcs.end(); ++it) {
        if (it->second.npc_id == pkt.target_npc_id) {
            target = (Character*)(uintptr_t)it->first;
            // Verify still in update list
            bool valid = false;
            ogre_unordered_set<Character*>::type::const_iterator cit;
            for (cit = chars.begin(); cit != chars.end(); ++cit) {
                if (*cit == target) { valid = true; break; }
            }
            if (!valid) target = NULL;
            break;
        }
    }

    if (!target) {
        KMP_LOG("[KenshiMP] Combat: target NPC " + itos(pkt.target_npc_id) + " not found");
        return;
    }

    Damages dmg(pkt.cut_damage, pkt.blunt_damage, pkt.pierce_damage, 0.0f, 0.0f);
    target->hitByMeleeAttack(CUT_DEFAULT, dmg, NULL, NULL, 0);

    KMP_LOG("[KenshiMP] Combat: applied damage to NPC " + itos(pkt.target_npc_id));
}

void host_sync_on_combat_stats(const PlayerCombatStats& pkt) {
    if (!s_is_host) return;

    Character* avatar = npc_manager_get_player_avatar(pkt.player_id);
    if (!avatar) {
        KMP_LOG("[KenshiMP] Combat stats: avatar not found for player " + itos(pkt.player_id));
        return;
    }

    CharStats* stats = avatar->getStats();
    if (!stats) {
        KMP_LOG("[KenshiMP] Combat stats: CharStats null for avatar");
        return;
    }

    stats->_strength = pkt.strength;
    stats->_dexterity = pkt.dexterity;
    stats->_toughness = pkt.toughness;
    stats->__meleeAttack = pkt.melee_attack;
    stats->_meleeDefence = pkt.melee_defence;
    stats->_athletics = pkt.athletics;

    KMP_LOG("[KenshiMP] Combat stats applied: str=" + itos(static_cast<uint32_t>(pkt.strength))
        + " dex=" + itos(static_cast<uint32_t>(pkt.dexterity))
        + " tgh=" + itos(static_cast<uint32_t>(pkt.toughness))
        + " atk=" + itos(static_cast<uint32_t>(pkt.melee_attack))
        + " def=" + itos(static_cast<uint32_t>(pkt.melee_defence)));
}

void host_sync_on_combat_target(const CombatTarget& pkt) {
    if (!s_is_host) return;
    if (!ou) return;

    Character* avatar = npc_manager_get_player_avatar(pkt.player_id);
    if (!avatar) {
        KMP_LOG("[KenshiMP] Combat target: avatar not found for player " + itos(pkt.player_id));
        return;
    }

    // Find the real NPC by npc_id
    Character* target = NULL;
    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
    std::map<uint64_t, SyncedNPC>::iterator it;
    for (it = s_synced_npcs.begin(); it != s_synced_npcs.end(); ++it) {
        if (it->second.npc_id == pkt.target_npc_id) {
            target = (Character*)(uintptr_t)it->first;
            bool valid = false;
            ogre_unordered_set<Character*>::type::const_iterator cit;
            for (cit = chars.begin(); cit != chars.end(); ++cit) {
                if (*cit == target) { valid = true; break; }
            }
            if (!valid) target = NULL;
            break;
        }
    }

    if (!target) {
        KMP_LOG("[KenshiMP] Combat target: NPC " + itos(pkt.target_npc_id) + " not found");
        return;
    }

    avatar->attackTarget(target);
    KMP_LOG("[KenshiMP] Combat: avatar attacking NPC " + itos(pkt.target_npc_id));
}

void host_sync_shutdown() {
    s_synced_npcs.clear();
}

void host_sync_set_host(bool is_host) {
    s_is_host = is_host;
    if (is_host) {
        KMP_LOG("[KenshiMP] This client is the HOST");
    }
}

bool host_sync_is_host() {
    return s_is_host;
}

uint32_t host_sync_get_synced_count() {
    return static_cast<uint32_t>(s_synced_npcs.size());
}

void host_sync_spawn_test_npc(float x, float y, float z) {
    if (!ou) return;
    if (!ou->player) return;

    RootObjectFactory* factory = ou->theFactory;
    if (!factory) return;

    // Use an empty/neutral faction so NPC doesn't join the player's team
    Faction* faction = ou->factionMgr->getEmptyFaction();

    Ogre::Vector3 spawn_pos(x, y, z);
    RootObjectBase* obj = factory->createRandomCharacter(
        faction, spawn_pos, NULL, NULL, NULL, 0.0f
    );

    Character* npc = dynamic_cast<Character*>(obj);
    if (npc) {
        // Don't null out AI — game still needs it for internal state
        // The NPC will wander on the host, but joiners control position

        // Add to synced set so it gets sent to joiners
        uint64_t key = make_npc_key(npc);
        uint32_t npc_id = s_next_npc_id++;

        SyncedNPC snpc;
        snpc.npc_id = npc_id;
        snpc.last_x = x;
        snpc.last_y = y;
        snpc.last_z = z;
        snpc.is_test = true;
        s_synced_npcs[key] = snpc;

        NPCSpawnRemote spawn;
        fill_spawn_packet(spawn, npc, npc_id);
        std::vector<uint8_t> buf = pack(spawn);
        client_send_reliable(buf.data(), buf.size());

        KMP_LOG(
            "[KenshiMP] Spawned test NPC " + itos(npc_id) + " at host position");
    }
}

// ---------------------------------------------------------------------------
// Check if an NPC is within sync radius of any player character
// ---------------------------------------------------------------------------
static bool is_in_range_of_any_player(const Ogre::Vector3& npc_pos) {
    if (!ou || !ou->player) return false;

    const lektor<Character*>& players = ou->player->getAllPlayerCharacters();
    for (int i = 0; i < players.count; ++i) {
        Character* pc = players.stuff[i];
        if (!pc) continue;
        Ogre::Vector3 player_pos = pc->getPosition();
        float dx = npc_pos.x - player_pos.x;
        float dy = npc_pos.y - player_pos.y;
        float dz = npc_pos.z - player_pos.z;
        float dist_sq = dx*dx + dy*dy + dz*dz;
        if (dist_sq <= NPC_SYNC_RADIUS * NPC_SYNC_RADIUS) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Fill spawn packet with NPC appearance data
// ---------------------------------------------------------------------------
static void fill_spawn_packet(NPCSpawnRemote& pkt, Character* ch, uint32_t npc_id) {
    pkt.npc_id = npc_id;

    // Name — use display name (GameData::name at offset 0x28)
    if (ch->data) {
        std::strncpy(pkt.name, ch->data->name.c_str(), MAX_NAME_LENGTH - 1);
        pkt.name[MAX_NAME_LENGTH - 1] = '\0';
    }

    // Race — send stringID so joiner can look it up with getRaceData()
    RaceData* race = ch->getRace();
    if (race && race->data) {
        std::strncpy(pkt.race, race->data->stringID.c_str(), MAX_RACE_LENGTH - 1);
        pkt.race[MAX_RACE_LENGTH - 1] = '\0';
    }

    // Position
    Ogre::Vector3 pos = ch->getPosition();
    pkt.x = pos.x;
    pkt.y = pos.y;
    pkt.z = pos.z;
    pkt.yaw = 0.0f;

    // Weapon and armour — left empty for now
    pkt.weapon[0] = '\0';
    pkt.armour[0] = '\0';
}

// ---------------------------------------------------------------------------
// Host sync tick — called every frame from player_sync_tick
// ---------------------------------------------------------------------------
void host_sync_tick(float dt) {
    if (!s_is_host) return;
    if (!ou) return;

    // Log once to confirm host sync is running
    static bool s_logged_first = false;
    if (!s_logged_first) {
        s_logged_first = true;
        KMP_LOG("[KenshiMP] host_sync_tick: first call");
    }

    // Get all active characters
    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();

    // Log character count periodically
    static int s_tick_count = 0;
    s_tick_count++;
    if (s_tick_count <= 3 || s_tick_count % 1000 == 0) {
        KMP_LOG("[KenshiMP] host_sync: chars=" + itos(static_cast<uint32_t>(chars.size())) +
            " synced=" + itos(static_cast<uint32_t>(s_synced_npcs.size())));
    }

    // Build list of NPCs currently in range
    std::vector<Character*> in_range;

    ogre_unordered_set<Character*>::type::const_iterator it;
    for (it = chars.begin(); it != chars.end(); ++it) {
        Character* ch = *it;
        if (!ch) continue;
        if (ch->isPlayerCharacter()) continue;
        if (npc_manager_is_player_npc(ch)) continue;  // skip remote player avatars

        Ogre::Vector3 pos = ch->getPosition();
        if (is_in_range_of_any_player(pos)) {
            in_range.push_back(ch);
        }
    }

    // New NPCs entering range → send NPC_SPAWN_REMOTE
    for (size_t i = 0; i < in_range.size(); ++i) {
        Character* ch = in_range[i];
        uint64_t key = make_npc_key(ch);

        if (s_synced_npcs.find(key) == s_synced_npcs.end()) {
            uint32_t npc_id = s_next_npc_id++;

            SyncedNPC snpc;
            snpc.npc_id = npc_id;
            Ogre::Vector3 pos = ch->getPosition();
            snpc.last_x = pos.x;
            snpc.last_y = pos.y;
            snpc.last_z = pos.z;
            snpc.is_test = false;
            s_synced_npcs[key] = snpc;

            NPCSpawnRemote spawn;
            fill_spawn_packet(spawn, ch, npc_id);
            std::vector<uint8_t> buf = pack(spawn);
            client_send_reliable(buf.data(), buf.size());
        }
    }

    // NPCs that left range → send NPC_DESPAWN_REMOTE
    std::map<uint64_t, bool> in_range_keys;
    for (size_t i = 0; i < in_range.size(); ++i) {
        in_range_keys[make_npc_key(in_range[i])] = true;
    }

    std::vector<uint64_t> to_remove;
    std::map<uint64_t, SyncedNPC>::iterator synced_it;
    for (synced_it = s_synced_npcs.begin(); synced_it != s_synced_npcs.end(); ++synced_it) {
        if (synced_it->second.is_test) continue;  // don't auto-despawn test NPCs
        if (in_range_keys.find(synced_it->first) == in_range_keys.end()) {
            NPCDespawnRemote despawn;
            despawn.npc_id = synced_it->second.npc_id;
            std::vector<uint8_t> buf = pack(despawn);
            client_send_reliable(buf.data(), buf.size());
            to_remove.push_back(synced_it->first);
        }
    }
    for (size_t i = 0; i < to_remove.size(); ++i) {
        s_synced_npcs.erase(to_remove[i]);
    }

    // Batch position updates at NPC_SYNC_INTERVAL (10Hz)
    s_npc_send_timer += dt;
    if (s_npc_send_timer < NPC_SYNC_INTERVAL) return;
    s_npc_send_timer = 0.0f;

    if (in_range.empty()) return;

    // Build batch packets
    std::vector<uint8_t> batch_buf;
    uint16_t count = 0;

    NPCBatchHeader batch_hdr;
    batch_buf.resize(sizeof(NPCBatchHeader));

    for (size_t i = 0; i < in_range.size(); ++i) {
        Character* ch = in_range[i];
        uint64_t key = make_npc_key(ch);
        std::map<uint64_t, SyncedNPC>::iterator found = s_synced_npcs.find(key);
        if (found == s_synced_npcs.end()) continue;

        Ogre::Vector3 pos = ch->getPosition();

        // Compute yaw from movement direction
        Ogre::Vector3 dir = ch->getMovementDirection();
        float yaw = 0.0f;
        if (dir.x != 0.0f || dir.z != 0.0f) {
            yaw = atan2(dir.x, dir.z);
        }

        NPCStateEntry entry;
        entry.npc_id = found->second.npc_id;
        entry.x = pos.x;
        entry.y = pos.y;
        entry.z = pos.z;
        entry.yaw = yaw;
        entry.speed = ch->getMovementSpeed();
        entry.animation_id = 0;

        // Health state — disabled for now, may crash on some NPCs
        // TODO: investigate safe way to read health
        entry.flags = 0;
        entry.health_percent = 100;

        size_t offset = batch_buf.size();
        batch_buf.resize(offset + sizeof(NPCStateEntry));
        std::memcpy(&batch_buf[offset], &entry, sizeof(NPCStateEntry));
        count++;

        if (count >= MAX_NPC_BATCH) {
            batch_hdr.count = count;
            std::memcpy(&batch_buf[0], &batch_hdr, sizeof(NPCBatchHeader));
            client_send_unreliable(batch_buf.data(), batch_buf.size());

            batch_buf.resize(sizeof(NPCBatchHeader));
            count = 0;
        }
    }

    if (count > 0) {
        batch_hdr.count = count;
        std::memcpy(&batch_buf[0], &batch_hdr, sizeof(NPCBatchHeader));
        client_send_unreliable(batch_buf.data(), batch_buf.size());
    }
}

} // namespace kmp
