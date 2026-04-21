// building_manager.cpp — Joiner-side: spawn/destroy puppet buildings
// from packets sent by the host.

#include <map>
#include <set>
#include <string>
#include <sstream>
#include <cstring>

#include <kenshi/Enums.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/Building/Building.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/util/hand.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreLogManager.h>
#include "kmp_log.h"

#include "packets.h"
#include "protocol.h"

namespace kmp {

static std::map<uint32_t, Building*> s_remote_buildings;   // building_id → puppet
static std::set<uint64_t>             s_remote_keys;       // Building* keys (for dedup with host scan)

static std::set<std::string>        s_skip_sids;

static std::string itos_bm(uint32_t v) {
    std::ostringstream ss; ss << v; return ss.str();
}

// Used by building_sync.cpp to skip buildings we spawned ourselves
// (avoids the echo loop where joiner's own remote buildings would be
// re-detected by the host scan and re-broadcast).
bool building_manager_is_remote(Building* b) {
    return s_remote_keys.find((uint64_t)(uintptr_t)b) != s_remote_keys.end();
}

void building_manager_init() {
    s_remote_buildings.clear();
    s_remote_keys.clear();
    s_skip_sids.clear();
}

void building_manager_shutdown() {
    s_remote_buildings.clear();
    s_remote_keys.clear();
    s_skip_sids.clear();
}

// ---------------------------------------------------------------------------
// Joiner-side wipe: destroy local world objects in small batches.
//
// Scan-every-tick mode: each tick re-scans the world and destroys up to
// BATCH_PER_TICK objects directly from the fresh results. No queue is held
// between ticks, so pointers can't go stale when the streaming system
// invalidates RootObject* as chunks page out.
//
// Skip-list grows from observed crashes: when a stringID crashes the game,
// add it to s_skip_sids and the joiner re-runs without that one.
// ---------------------------------------------------------------------------

static bool   s_wipe_active   = false;
static float  s_wipe_timer    = 0.0f;
static const float WIPE_INTERVAL    = 0.1f;     // 100ms between destroys
static const int   BATCH_PER_TICK   = 1;        // ONE per scan — cascade-safe

static const char* type_name(int t) {
    if (t == BUILDING) return "BUILDING";
    if (t == ITEM)     return "ITEM";
    return "UNKNOWN";
}

// Read GameData::stringID safely. obj may be Building or Item — both expose
// `data` field at the same offset (RootObject base) per Building.h:166. We
// don't include Item.h (static-init crash). Read via dynamic_cast on Building
// for now; for ITEM types we skip the SID snapshot.
static std::string snapshot_sid(RootObject* obj, int type) {
    if (type == BUILDING) {
        Building* b = dynamic_cast<Building*>(obj);
        if (b) {
            GameData* gd = b->getGameData();
            if (gd) return gd->stringID;
        }
    }
    return std::string("(no-sid)");
}

// Scan one itemType, find the closest eligible object, and destroy it.
// Only ONE object per call — avoids cascade invalidation within a batch.
// Returns 1 if destroyed, 0 if nothing found.
static int scan_and_destroy(int type) {
    if (!ou || !ou->player) return 0;

    const lektor<Character*>& players = ou->player->getAllPlayerCharacters();
    if (players.count == 0) return 0;
    Character* anchor = players.stuff[0];
    if (!anchor) return 0;
    Ogre::Vector3 center = anchor->getPosition();

    lektor<RootObject*> results;
    ou->getObjectsWithinSphere(results, center, 50000.0f, (itemType)type, 99999, NULL);

    // Find the closest eligible object
    RootObject* closest = NULL;
    float closest_dist2 = 1e30f;
    std::string closest_sid;

    for (uint32_t i = 0; i < results.count; ++i) {
        RootObject* obj = results.stuff[i];
        if (!obj) continue;
        if (s_remote_keys.find((uint64_t)(uintptr_t)obj) != s_remote_keys.end()) continue;

        std::string sid = snapshot_sid(obj, type);
        if (s_skip_sids.find(sid) != s_skip_sids.end()) continue;

        Ogre::Vector3 pos = obj->getPosition();
        float dist2 = center.squaredDistance(pos);
        if (dist2 < closest_dist2) {
            closest = obj;
            closest_dist2 = dist2;
            closest_sid = sid;
        }
    }

    if (!closest) return 0;

    KMP_LOG(std::string("[KenshiMP] DESTROY type=") + type_name(type)
        + " ptr=" + itos_bm((uint32_t)((uint64_t)(uintptr_t)closest & 0xFFFFFFFF))
        + " sid='" + closest_sid + "'");

    if (type == BUILDING) {
        // Use the game's native building demolition path — handles
        // physics/collision cleanup that ou->destroy() misses.
        hand h(static_cast<RootObjectBase*>(closest));
        ou->dynamicDestroyBuilding(h);
    } else {
        ou->destroy(closest, false, "KenshiMP");
    }

    KMP_LOG("[KenshiMP] DESTROY ok");
    return 1;
}

void building_manager_hide_local_buildings() {
    KMP_LOG("[KenshiMP] Wipe active (scan-every-tick mode, batch="
        + itos_bm((uint32_t)BATCH_PER_TICK) + " per "
        + itos_bm((uint32_t)(WIPE_INTERVAL*1000)) + "ms)");
    s_wipe_active = true;
    s_wipe_timer = 0.0f;
}

void building_manager_show_local_buildings() {
    s_wipe_active = false;
    KMP_LOG("[KenshiMP] Wipe deactivated (destroyed objects NOT restored — reload save)");
}

void building_manager_wipe_tick(float dt) {
    if (!s_wipe_active) return;
    if (!ou) return;

    s_wipe_timer += dt;
    if (s_wipe_timer < WIPE_INTERVAL) return;
    s_wipe_timer = 0.0f;

    // Destroy one object per tick — items first, then buildings.
    // Only one per scan prevents cascade from invalidating other results.
    if (!scan_and_destroy(ITEM))
        scan_and_destroy(BUILDING);
}

uint32_t building_manager_get_remote_count() {
    return static_cast<uint32_t>(s_remote_buildings.size());
}

void building_manager_on_remote_spawn(const BuildingSpawnRemote& pkt) {
    if (!ou) return;
    if (!ou->theFactory) return;

    // Already spawned?
    if (s_remote_buildings.find(pkt.building_id) != s_remote_buildings.end()) return;

    // Look up GameData by stringID
    GameDataManager& gdm = ou->gamedata;
    std::string sid(pkt.stringID);
    boost::unordered::unordered_map<std::string, GameData*, boost::hash<std::string>,
        std::equal_to<std::string>,
        Ogre::STLAllocator<std::pair<std::string const, GameData*>, Ogre::GeneralAllocPolicy>
    >::iterator it = gdm.gamedataSID.find(sid);
    if (it == gdm.gamedataSID.end() || !it->second) {
        KMP_LOG("[KenshiMP] Building spawn: GameData not found for sid='" + sid + "'");
        return;
    }
    GameData* gd = it->second;

    Ogre::Vector3 pos(pkt.x, pkt.y, pkt.z);
    float qlen2 = pkt.qw*pkt.qw + pkt.qx*pkt.qx + pkt.qy*pkt.qy + pkt.qz*pkt.qz;
    Ogre::Quaternion rot = (qlen2 < 1e-6f)
        ? Ogre::Quaternion::IDENTITY
        : Ogre::Quaternion(pkt.qw, pkt.qx, pkt.qy, pkt.qz);

    // Some specialised building types (Farm, Turret, Crafting, ...) refuse
    // to construct without an owner Faction. Try NULL first (keeps "not
    // player-owned"); if that fails fall back to a neutral faction; last
    // resort is the player faction so at least *something* spawns.
    auto try_create = [&](Faction* owner) -> Building* {
        return ou->theFactory->createBuilding(
            gd, pos, NULL, owner, rot,
            NULL, NULL, NULL, NULL, NULL,
            false,
            pkt.completed != 0,
            pkt.is_foliage != 0,
            static_cast<int>(pkt.floor),
            false);
    };

    Faction* neutral = NULL;
    if (ou->factionMgr) {
        const char* try_names[] = { "Wanderers", "Drifters", "Wilderness", "neutral", NULL };
        for (int i = 0; try_names[i] && !neutral; ++i) {
            neutral = ou->factionMgr->getFactionByName(std::string(try_names[i]));
            if (!neutral) neutral = ou->factionMgr->getFactionByStringID(std::string(try_names[i]));
        }
    }

    Building* b = try_create(NULL);
    if (!b && neutral) { b = try_create(neutral); }
    if (!b && ou->player) { b = try_create(ou->player->getFaction()); }

    if (!b) {
        KMP_LOG("[KenshiMP] Building spawn: createBuilding returned NULL for sid='" + sid + "' (tried NULL, neutral, player factions)");
        return;
    }

    b->setVisible(true);

    // Only build the physical representation if the constructor didn't already
    // — blindly calling createPhysical() a second time has crashed the game.
    // isPhysical() on the Building tells us the current state.
    if (!b->_NV_isPhysical()) {
        b->_NV_createPhysical();
    }
    b->notifyChange();
    if (pkt.completed != 0) {
        b->_NV_notifyConstructionComplete();
    }

    s_remote_buildings[pkt.building_id] = b;
    s_remote_keys.insert((uint64_t)(uintptr_t)b);

    KMP_LOG("[KenshiMP] Spawned remote building id=" + itos_bm(pkt.building_id) + " sid='" + sid + "'");
}

void building_manager_on_remote_despawn(uint32_t building_id) {
    if (!ou) return;

    std::map<uint32_t, Building*>::iterator it = s_remote_buildings.find(building_id);
    if (it == s_remote_buildings.end()) return;

    Building* b = it->second;
    s_remote_buildings.erase(it);
    if (b) {
        s_remote_keys.erase((uint64_t)(uintptr_t)b);
        ou->destroy(b, false, "KenshiMP");
        KMP_LOG("[KenshiMP] Despawned remote building id=" + itos_bm(building_id));
    }
}

} // namespace kmp
