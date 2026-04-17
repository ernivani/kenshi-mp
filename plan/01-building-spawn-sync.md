# Phase 1 — Building Spawn Sync

## Goal
When host (or any player) places a building (campfire, sleeping bag, walls, gates, etc.) the building appears in the world of all connected clients at the same position/rotation/state.

## Packets (`common/include/packets.h`)

```cpp
static const uint8_t BUILDING_SPAWN_REMOTE   = 0x60;  // reliable
static const uint8_t BUILDING_DESPAWN_REMOTE = 0x62;  // reliable
```

`BuildingSpawnRemote`:
```
uint32_t building_id;
char     stringID[MAX_STRINGID_LENGTH];
float    x, y, z;
float    qw, qx, qy, qz;
uint8_t  completed;
uint8_t  is_foliage;
int16_t  floor;
uint32_t faction_hash;  // 0 = host's player faction
```

`BuildingDespawnRemote`:
```
uint32_t building_id;
```

## Host side — `core/src/building_sync.cpp`
Mirror `host_sync.cpp`. Two detection options:

1. **Hook `RootObjectFactory::createBuilding`** (preferred) via `KenshiLib::AddHook`. On call: assign `uint32_t id` from sequential counter, capture args, send `BUILDING_SPAWN_REMOTE`.
2. **Per-tick proximity scan** (fallback) — `getObjectsWithinSphere(itemType::BUILDING, radius)` around each player, diff against `std::map<uint64_t ptr, uint32_t id>`.

Resend-on-join: when new joiner connects, walk tracked buildings and send all their `BUILDING_SPAWN_REMOTE` packets.

`createBuilding` signature (from `RootObjectFactory.h:37`):
```
Building* createBuilding(GameData*, Ogre::Vector3, TownBase*, Faction*,
                         Ogre::Quaternion, callback, Layout*, Building* isDoorOf,
                         GameSaveState*, Building* isIndoorsOf, bool invisible,
                         bool completed, bool isFoliage, int floorNumber,
                         bool isOutsideFurniture)
```

## Joiner side — `core/src/building_manager.cpp`
On `BUILDING_SPAWN_REMOTE`:
- Look up `GameData*` by `stringID` in `ou->gamedata.gamedataSID`
- Call `factory->createBuilding(gd, pos, NULL, faction, rot, NULL, NULL, NULL, NULL, NULL, false, completed, is_foliage, floor, false)`
- Store `Building* → building_id` map

On `BUILDING_DESPAWN_REMOTE`:
- `ou->destroy(b, false, "KenshiMP")` or `ou->dynamicDestroyBuilding(hand)`

## Server side — `server/src/session.cpp`
Extend packet-type switch to relay `BUILDING_SPAWN_REMOTE` and `BUILDING_DESPAWN_REMOTE` host↔joiner.
Add building tracker (mirror NPC tracker) so joiners get full resend on join.

## Files

- **EDIT:** `common/include/packets.h`, `core/src/player_sync.cpp` (dispatch), `server/src/session.cpp`, `core/CMakeLists.txt`
- **CREATE:** `core/src/building_sync.cpp`, `core/src/building_manager.cpp`

## Risks / unknowns (probe at runtime)

1. **Hook mangled symbol** — need to find `?createBuilding@RootObjectFactory@@...` in KenshiLib.dll exports
2. **Faction passed to createBuilding** on joiner — using player's own faction may trigger construction-UI; may need `completed=true` always
3. **Quaternion rotation stability** — placement physics may reject angle on joiner; might need post-spawn position fixup
4. **No teleport/move building API surfaced** — if rotation off, may need direct `pos` write via offset

## Test plan

1. Host spawns campfire on flat ground → joiner sees it at same pos/rot
2. Host spawns sleeping bag → joiner sees it
3. Host destroys campfire → joiner's despawns
4. Joiner reconnects → all existing buildings resent on join
