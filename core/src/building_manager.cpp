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
static std::vector<Building*>         s_hidden_locals;     // local buildings hidden on connect

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
    s_hidden_locals.clear();
}

void building_manager_shutdown() {
    s_remote_buildings.clear();
    s_remote_keys.clear();
    s_hidden_locals.clear();
}

// ---------------------------------------------------------------------------
// Hide all locally-existing buildings on join — joiner only sees host's world.
// Symmetric to npc_manager_hide_local_npcs(). Reversible via show().
// ---------------------------------------------------------------------------
void building_manager_hide_local_buildings() {
    if (!ou) return;
    if (!ou->player) return;

    const lektor<Character*>& players = ou->player->getAllPlayerCharacters();
    if (players.count == 0) return;
    Character* anchor = players.stuff[0];
    if (!anchor) return;
    Ogre::Vector3 center = anchor->getPosition();

    lektor<RootObject*> results;
    ou->getObjectsWithinSphere(results, center, 50000.0f, BUILDING, 9999, NULL);

    int count = 0;
    for (uint32_t i = 0; i < results.count; ++i) {
        Building* b = dynamic_cast<Building*>(results.stuff[i]);
        if (!b) continue;
        // Skip any building we already track as a remote puppet
        if (s_remote_keys.find((uint64_t)(uintptr_t)b) != s_remote_keys.end()) continue;
        b->setVisible(false);
        s_hidden_locals.push_back(b);
        count++;
    }
    KMP_LOG("[KenshiMP] Hidden " + itos_bm((uint32_t)count) + " local buildings");
}

void building_manager_show_local_buildings() {
    int count = 0;
    for (size_t i = 0; i < s_hidden_locals.size(); ++i) {
        Building* b = s_hidden_locals[i];
        if (b) {
            b->setVisible(true);
            count++;
        }
    }
    s_hidden_locals.clear();
    KMP_LOG("[KenshiMP] Restored " + itos_bm((uint32_t)count) + " local buildings");
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
