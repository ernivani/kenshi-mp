// building_manager.cpp — Joiner-side: spawn/destroy puppet buildings
// from packets sent by the host.

#include <map>
#include <set>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>

#include <kenshi/Enums.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/Building.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/RootObjectBase.h>
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

struct PendingDestroy {
    RootObject* obj;
    int         type;
    std::string sid_snapshot;
};
static std::vector<PendingDestroy>  s_destroy_queue;
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
    s_destroy_queue.clear();
    s_skip_sids.clear();
}

void building_manager_shutdown() {
    s_remote_buildings.clear();
    s_remote_keys.clear();
    s_destroy_queue.clear();
    s_skip_sids.clear();
}

// ---------------------------------------------------------------------------
// Joiner-side wipe: destroy local world objects in small batches with
// per-object logging so the last log line before a crash names the culprit.
// Bulk destroy is the goal but crashes randomly — instrumented mode below.
//
// Strategy:
//   1. Build queue of (RootObject*, type, stringID-snapshot) at hide() time
//   2. Each tick, destroy up to BATCH_PER_TICK objects from the queue
//   3. Log EACH destroy attempt BEFORE the call so the crash log shows
//      the last attempted object
//   4. Once queue is drained, switch to scan-mode that catches newly
//      streamed chunks
//
// Skip-list grows from observed crashes: when a stringID crashes the game,
// add it to s_skip_sids and the joiner re-runs without that one.
// ---------------------------------------------------------------------------

static bool   s_wipe_active   = false;
static float  s_wipe_timer    = 0.0f;
static const float WIPE_INTERVAL    = 0.5f;
static const int   BATCH_PER_TICK   = 5;        // tiny batches → easy to bisect

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

static void enqueue_objects_of_type(int type, int& count_out) {
    if (!ou || !ou->player) return;
    const lektor<Character*>& players = ou->player->getAllPlayerCharacters();
    if (players.count == 0) return;
    Character* anchor = players.stuff[0];
    if (!anchor) return;
    Ogre::Vector3 center = anchor->getPosition();

    lektor<RootObject*> results;
    ou->getObjectsWithinSphere(results, center, 50000.0f, (itemType)type, 99999, NULL);

    for (uint32_t i = 0; i < results.count; ++i) {
        RootObject* obj = results.stuff[i];
        if (!obj) continue;
        if (s_remote_keys.find((uint64_t)(uintptr_t)obj) != s_remote_keys.end()) continue;

        std::string sid = snapshot_sid(obj, type);
        if (s_skip_sids.find(sid) != s_skip_sids.end()) continue;

        PendingDestroy pd;
        pd.obj  = obj;
        pd.type = type;
        pd.sid_snapshot = sid;
        s_destroy_queue.push_back(pd);
        count_out++;
    }
}

void building_manager_hide_local_buildings() {
    s_destroy_queue.clear();
    int buildings = 0, items = 0;
    enqueue_objects_of_type(BUILDING, buildings);
    enqueue_objects_of_type(ITEM,     items);
    KMP_LOG("[KenshiMP] Wipe: queued " + itos_bm((uint32_t)buildings) + " buildings + "
        + itos_bm((uint32_t)items) + " items for destroy (batches of "
        + itos_bm((uint32_t)BATCH_PER_TICK) + " per " + itos_bm((uint32_t)(WIPE_INTERVAL*1000)) + "ms)");
    s_wipe_active = true;
    s_wipe_timer = 0.0f;
}

void building_manager_show_local_buildings() {
    s_destroy_queue.clear();
    s_wipe_active = false;
    KMP_LOG("[KenshiMP] Wipe deactivated (destroyed objects NOT restored — reload save)");
}

// Called every frame while connected. Destroys up to BATCH_PER_TICK queued
// objects per WIPE_INTERVAL. Each destroy is logged BEFORE the call so the
// crash log identifies the offending object.
void building_manager_wipe_tick(float dt) {
    if (!s_wipe_active) return;
    if (!ou) return;

    s_wipe_timer += dt;
    if (s_wipe_timer < WIPE_INTERVAL) return;
    s_wipe_timer = 0.0f;

    // Top up queue by re-scanning (catches newly streamed chunks)
    if (s_destroy_queue.empty()) {
        int b = 0, i = 0;
        enqueue_objects_of_type(BUILDING, b);
        enqueue_objects_of_type(ITEM,     i);
        if (b + i > 0) {
            KMP_LOG("[KenshiMP] Wipe scan: enqueued " + itos_bm((uint32_t)b) + " buildings + "
                + itos_bm((uint32_t)i) + " items from streamed chunks");
        }
    }

    int destroyed = 0;
    while (destroyed < BATCH_PER_TICK && !s_destroy_queue.empty()) {
        PendingDestroy pd = s_destroy_queue.back();
        s_destroy_queue.pop_back();

        if (!pd.obj) continue;

        KMP_LOG(std::string("[KenshiMP] DESTROY type=") + type_name(pd.type)
            + " ptr=" + itos_bm((uint32_t)((uint64_t)(uintptr_t)pd.obj & 0xFFFFFFFF))
            + " sid='" + pd.sid_snapshot + "'");
        ou->destroy(pd.obj, false, "KenshiMP");
        KMP_LOG(std::string("[KenshiMP] DESTROY ok"));
        destroyed++;
    }
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
    Ogre::Quaternion rot(pkt.qw, pkt.qx, pkt.qy, pkt.qz);

    // Faction = NULL: let the game default it. We can refine later
    // (e.g. mirror the host player's faction) once we sync faction info.
    Faction* faction = NULL;

    Building* b = ou->theFactory->createBuilding(
        gd, pos, NULL, faction, rot,
        NULL,           // FactoryCallbackInterface
        NULL,           // Layout (furnitureOf)
        NULL,           // isDoorOf
        NULL,           // GameSaveState
        NULL,           // isIndoorsOf
        false,          // invisible
        pkt.completed != 0,
        pkt.is_foliage != 0,
        static_cast<int>(pkt.floor),
        false           // isOutsideFurniture
    );

    if (!b) {
        KMP_LOG("[KenshiMP] Building spawn: createBuilding returned NULL for sid='" + sid + "'");
        return;
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
