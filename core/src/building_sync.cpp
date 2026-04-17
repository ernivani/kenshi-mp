// building_sync.cpp — Host-side building scan and spawn/despawn sync
//
// Phase 1 strategy: periodic global scan via getObjectsWithinSphere with a
// huge radius. No proximity filter — buildings are static and rare enough
// that all of them get synced regardless of distance.
//
// Phase 1.5 (TODO): replace the scan with a hook on
// RootObjectFactory::createBuilding for instant capture.

#include <map>
#include <vector>
#include <set>
#include <cstring>
#include <string>
#include <sstream>

#include <kenshi/Enums.h>
#include <kenshi/GameData.h>
#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Building.h>
#include <kenshi/Character.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectBase.h>
#include <OgreVector3.h>
#include <OgreLogManager.h>
#include "kmp_log.h"

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

extern void client_send_reliable(const uint8_t* data, size_t length);
extern bool host_sync_is_host();
extern bool building_manager_is_remote(Building* b);  // defined in building_manager.cpp

static std::string itos_b(uint32_t val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Synced building map: Building* → uint32_t building_id
// ---------------------------------------------------------------------------
struct SyncedBuilding {
    uint32_t building_id;
    // cached snapshot for spawn packet (in case the building disappears
    // between scan and resend-on-join)
    BuildingSpawnRemote spawn_pkt;
};

static std::map<uint64_t, SyncedBuilding> s_synced_buildings;
static std::set<uint64_t>                 s_baseline_keys;     // pre-existing world buildings — NEVER synced
static bool                               s_baseline_captured = false;
static uint32_t s_next_building_id = 1;
static float    s_scan_timer = 0.0f;

static uint64_t make_key(Building* b) {
    return (uint64_t)(uintptr_t)b;
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void building_sync_init() {
    s_synced_buildings.clear();
    s_baseline_keys.clear();
    s_baseline_captured = false;
    s_next_building_id = 1;
    s_scan_timer = 0.0f;
}

void building_sync_shutdown() {
    s_synced_buildings.clear();
    s_baseline_keys.clear();
    s_baseline_captured = false;
}

uint32_t building_sync_get_synced_count() {
    return static_cast<uint32_t>(s_synced_buildings.size());
}

// ---------------------------------------------------------------------------
// Pack a Building's current state into a spawn packet
// ---------------------------------------------------------------------------
static bool fill_spawn_packet(BuildingSpawnRemote& pkt, Building* b, uint32_t id) {
    GameData* gd = b->getGameData();
    if (!gd) return false;

    pkt.building_id = id;

    std::strncpy(pkt.stringID, gd->stringID.c_str(), MAX_STRINGID_LENGTH - 1);
    pkt.stringID[MAX_STRINGID_LENGTH - 1] = '\0';

    Ogre::Vector3 pos = b->getPosition();
    pkt.x = pos.x; pkt.y = pos.y; pkt.z = pos.z;

    const Ogre::Quaternion& q = b->getOrientation();
    pkt.qw = q.w; pkt.qx = q.x; pkt.qy = q.y; pkt.qz = q.z;

    pkt.completed   = 1;  // assume complete on first sync (state sync = phase 2)
    pkt.is_foliage  = b->isFoliage ? 1 : 0;
    pkt.floor       = static_cast<int16_t>(b->getFloor());
    return true;
}

// ---------------------------------------------------------------------------
// Resend all tracked buildings — called when a new joiner connects
// ---------------------------------------------------------------------------
void building_sync_resend_all() {
    if (!host_sync_is_host()) return;

    int count = 0;
    std::map<uint64_t, SyncedBuilding>::iterator it;
    for (it = s_synced_buildings.begin(); it != s_synced_buildings.end(); ++it) {
        std::vector<uint8_t> buf = pack(it->second.spawn_pkt);
        client_send_reliable(buf.data(), buf.size());
        count++;
    }
    KMP_LOG("[KenshiMP] Resent " + itos_b(count) + " synced buildings to new joiner");
}

// ---------------------------------------------------------------------------
// Scan + diff — called periodically from building_sync_tick
// ---------------------------------------------------------------------------
static void scan_and_diff() {
    if (!ou) return;
    if (!ou->player) return;

    // Center the scan on the host's player position. Use a huge radius so
    // we effectively get all buildings on the map.
    const lektor<Character*>& players = ou->player->getAllPlayerCharacters();
    if (players.count == 0) return;
    Character* anchor = players.stuff[0];
    if (!anchor) return;
    Ogre::Vector3 center = anchor->getPosition();

    lektor<RootObject*> results;
    ou->getObjectsWithinSphere(results, center, 50000.0f, BUILDING, 9999, NULL);

    // First scan: capture every existing building as baseline. These are
    // pre-existing world buildings that should NOT be synced — joiner only
    // sees the empty world + buildings the host places after this point.
    if (!s_baseline_captured) {
        for (uint32_t i = 0; i < results.count; ++i) {
            RootObject* obj = results.stuff[i];
            if (!obj) continue;
            Building* b = dynamic_cast<Building*>(obj);
            if (!b) continue;
            s_baseline_keys.insert(make_key(b));
        }
        s_baseline_captured = true;
        KMP_LOG("[KenshiMP] Building baseline captured: " + itos_b((uint32_t)s_baseline_keys.size()) + " pre-existing buildings ignored from sync");
        return;
    }

    // Mark which non-baseline keys are still present this scan
    std::set<uint64_t> seen_keys;

    for (uint32_t i = 0; i < results.count; ++i) {
        RootObject* obj = results.stuff[i];
        if (!obj) continue;
        Building* b = dynamic_cast<Building*>(obj);
        if (!b) continue;

        uint64_t key = make_key(b);

        // Skip pre-existing world buildings
        if (s_baseline_keys.find(key) != s_baseline_keys.end()) continue;
        // Skip buildings spawned by the joiner-spawn path (echo loop guard)
        if (building_manager_is_remote(b)) continue;

        seen_keys.insert(key);

        if (s_synced_buildings.find(key) == s_synced_buildings.end()) {
            uint32_t bid = s_next_building_id++;
            SyncedBuilding sb;
            sb.building_id = bid;
            if (!fill_spawn_packet(sb.spawn_pkt, b, bid)) continue;
            s_synced_buildings[key] = sb;

            std::vector<uint8_t> buf = pack(sb.spawn_pkt);
            client_send_reliable(buf.data(), buf.size());
            KMP_LOG("[KenshiMP] Building spawn id=" + itos_b(bid) + " sid=" + std::string(sb.spawn_pkt.stringID));
        }
    }

    // Player-placed buildings tracked but not seen this scan → despawn
    std::vector<uint64_t> to_remove;
    std::map<uint64_t, SyncedBuilding>::iterator it;
    for (it = s_synced_buildings.begin(); it != s_synced_buildings.end(); ++it) {
        if (seen_keys.find(it->first) == seen_keys.end()) {
            BuildingDespawnRemote despawn;
            despawn.building_id = it->second.building_id;
            std::vector<uint8_t> buf = pack(despawn);
            client_send_reliable(buf.data(), buf.size());
            to_remove.push_back(it->first);
            KMP_LOG("[KenshiMP] Building despawn id=" + itos_b(despawn.building_id));
        }
    }
    for (size_t i = 0; i < to_remove.size(); ++i) {
        s_synced_buildings.erase(to_remove[i]);
    }
}

// ---------------------------------------------------------------------------
// Tick — called every frame from player_sync_tick
// ---------------------------------------------------------------------------
void building_sync_tick(float dt) {
    if (!host_sync_is_host()) return;
    if (!ou) return;

    s_scan_timer += dt;
    if (s_scan_timer < BUILDING_SCAN_INTERVAL) return;
    s_scan_timer = 0.0f;

    scan_and_diff();
}

} // namespace kmp
