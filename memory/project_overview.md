---
name: KenshiMP Project Overview
description: Complete state of the Kenshi multiplayer mod — architecture, what works, what doesn't, all lessons learned
type: project
---

## What Is KenshiMP

A multiplayer mod for Kenshi (single-player RPG) built from scratch in C++. Host/join model where one player's game is authoritative and others connect to see the same world.

## Architecture

```
Host Kenshi (v100 DLL)
  └─ RE_Kenshi loads KenshiMP.dll
       ├─ Game loop hooked via KenshiLib::AddHook
       ├─ host_sync.cpp scans NPCs → sends to server
       ├─ player_sync.cpp sends player position
       └─ MyGUI overlay (F8 connect, F10 admin)
                ↕ (ENet UDP via dedicated server)
Joiner Kenshi (v100 DLL)
  └─ Same DLL, different role
       ├─ Receives NPC spawn/state/despawn
       ├─ npc_manager.cpp spawns puppet NPCs
       ├─ Hides local NPCs (teleport underground)
       └─ Players see each other walking
```

**Server:** Standalone VS2022 C++17 exe. Relays packets, tracks host, manages sessions. Auto-launched by Host button.

## Build Setup

### Core DLL (v100 — MUST use VS2010 toolset):
```bash
cmake -B build -T v100 -A x64 \
  -DENET_DIR=deps/enet2 \
  -DKENSHILIB_DIR=deps/KenshiLib \
  -DKENSHILIB_EXAMPLES_DEPS=deps/KenshiLib_Examples_deps \
  -DBOOST_ROOT=deps/KenshiLib_Examples_deps/boost_1_60_0 \
  -DKENSHIMP_BUILD_CORE=ON -DKENSHIMP_BUILD_SERVER=OFF -DKENSHIMP_BUILD_INJECTOR=OFF
cmake --build build --config Release --target KenshiMP
```

### Server (VS2022):
```bash
cmake -B build_server -A x64 -DENET_DIR=deps/enet2 \
  -DKENSHIMP_BUILD_CORE=OFF -DKENSHIMP_BUILD_SERVER=ON -DKENSHIMP_BUILD_INJECTOR=OFF
cmake --build build_server --config Release
```

### Deploy:
```bash
cp build/core/Release/KenshiMP.dll "mods/KenshiMP/"
cp build_server/server/Release/kenshi-mp-server.exe "mods/KenshiMP/"
```

## v100 Toolset Rules (CRITICAL)
- No `enum class` → namespace with `static const uint8_t`
- No `constexpr` → `static const`
- No `nullptr` → `NULL`
- No `auto` → explicit iterator types
- No `std::function` → typedef function pointer
- No `<mutex>`, `<queue>` — not in v100 STL
- `std::to_string(uint32_t)` ambiguous → use `std::ostringstream`
- `#include <kenshi/Item.h>` causes static init crash → use forward declaration from Character.h
- Use `KMP_LOG(msg)` macro (writes to per-instance log + Ogre log)

## What Works

### Networking
- ENet UDP client/server, 20Hz player state, 20Hz NPC batch state
- Auto-reconnect every 3 seconds
- 60 second timeout (handles alt-tab)
- `enet_host_flush()` after every send

### Game Loop Hook
- `GetProcAddress(GetModuleHandleA("KenshiLib.dll"), "?mainLoop_GPUSensitiveStuff@GameWorld@@UEAAXM@Z")` → `KenshiLib::GetRealAddress(stub)` → `AddHook`
- Subsystem init deferred to first hook call (MyGUI not ready during startPlugin)
- `_NV_` stubs are empty (zero bytes) — can't use with GetRealAddress
- Virtual member function pointers contain vtable indices — can't use directly

### Host NPC Sync (Phase 1)
- Host scans `ou->getCharacterUpdateList()` every tick
- Proximity filter: 500 units from any player
- NPC ID: Character* pointer → sequential uint32_t
- NPC_SPAWN_REMOTE (reliable), NPC_BATCH_STATE (20Hz unreliable), NPC_DESPAWN_REMOTE (reliable)
- Resends all synced NPCs when new joiner connects
- Skips player avatars in NPC scan (`npc_manager_is_player_npc()`)

### NPC Spawning on Joiner
- `createRandomCharacter(Drifters_faction, pos, NULL, NULL, NULL, 0.0f)` — spawns neutral NPCs
- `setDestination(target, HIGH_PRIORITY, false)` — natural walking animation
- `teleport(pos, rot)` only for large distances (>50/100 units)
- Local NPCs hidden (teleported underground, continuous hide with std::set protection)

### UI
- F8: Host/Join/Disconnect dialog (MyGUI)
- F10: Admin panel — player list, synced NPC count, Spawn Test NPC, Give Items
- Status text: "HOSTING as Player #N" / "JOINED as Player #N"
- Host button auto-launches server exe as child process
- Base skins (`Kenshi_TextBoxEmptySkin`, `Kenshi_EditBoxEmptySkin`, `Kenshi_Button1Skin`) + explicit `setFontName`/`setTextColour`

### Item Giving (Partial)
- Search by `GameData::name` (offset 0x28), NOT `stringID` (composite key)
- `factory->createItem(gd, hand(), NULL, NULL, -1, NULL)` — works for food/armor
- Working: Dried Meat, Standard first aid kit, Armoured Rags
- Failing: Weapons (Katana, Plank) — need weapon mesh GameData

## What DOESN'T Work (Lessons Learned)

### DO NOT:
- `npc->ai = NULL` → crash next frame (game update loop references AI)
- `movement->halt()` every frame → cancels setDestination, NPCs twitch/teleport
- `factory->create(GameData*, ...)` for puppet NPCs → full AI enabled, uncontrollable
- `getOrCreateFaction("custom")` → crash createRandomCharacter (empty faction)
- `getEmptyFaction()` → crash createRandomCharacter
- `#include <kenshi/Item.h>` → static init crash in v100

### USE INSTEAD:
- `setDestination(HIGH_PRIORITY)` → overrides AI destination naturally
- `Drifters` faction → neutral, has character templates, works with createRandomCharacter
- `createRandomCharacter` → minimal AI that doesn't fight setDestination
- Forward-declare `Item*` from Character.h for item operations

## Key KenshiLib API

### Game Access
- `ou` — global GameWorld singleton (from kenshi/Globals.h)
- `ou->player->getAllPlayerCharacters()` — lektor with .stuff[] and .count
- `ou->getCharacterUpdateList()` — all active Characters
- `ou->theFactory` — RootObjectFactory for spawning
- `ou->factionMgr` — FactionManager
- `ou->gamedata.gamedataSID` — map of all game data (55K+ entries)

### Character
- `ch->getPosition()` → Ogre::Vector3
- `ch->getMovementDirection()` → Ogre::Vector3 (for yaw: `atan2(dir.x, dir.z)`)
- `ch->getMovementSpeed()` → float
- `ch->isPlayerCharacter()` → bool
- `ch->getRace()` → RaceData*
- `ch->getMovement()` → CharMovement*
- `ch->giveItem(item, true, false)` — add item to inventory
- `ch->data->name` — display name (offset 0x28)
- `ch->data->stringID` — composite key like "576-gamedata.base.TECH.1"

### Movement
- `movement->setDestination(Vector3, HIGH_PRIORITY, false)` — pathfind + walk
- `movement->halt()` — stop current movement (use sparingly)
- `movement->isIdle()` / `movement->isRunning()`

### Spawning
- `factory->createRandomCharacter(faction, pos, NULL, NULL, NULL, 0.0f)` — random NPC
- `factory->createItem(gd, hand(), NULL, NULL, -1, NULL)` — create item
- `factory->create(GameData*, pos, false, faction, rot, NULL, NULL, NULL, false, NULL, 0.0f)` — specific template (but gives full AI)

### Factions
- `factionMgr->getFactionByName("Drifters")` — find neutral faction (capital D!)
- `player->getFaction()` — player's faction (NPCs join team)

## File Map

### Core DLL (v100)
- `plugin.cpp` — entry point, game loop hook via GetProcAddress + AddHook
- `game_hooks.cpp` — KenshiLib accessor wrappers (ou->player, ou->theFactory, etc.)
- `player_sync.cpp` — main tick loop, packet dispatch, auto-reconnect, host_sync wiring
- `npc_manager.cpp` — player NPC spawn/move, remote NPC spawn/state/despawn, local NPC hiding
- `host_sync.cpp` — host-side NPC scanning, ID assignment, batch sending, resend on join
- `admin_panel.cpp` — F10 admin panel (MyGUI)
- `ui.cpp` — F8 connect dialog, status text, embedded server launch
- `client.cpp` — ENet client networking
- `kmp_log.h` — per-instance logging with PID

### Server (VS2022)
- `server/src/main.cpp` — entry point, ENet server loop
- `server/src/session.cpp` — session management, host tracking, NPC packet relay
- `server/src/relay.cpp` — broadcast/unicast helpers
- `server/src/world_state.cpp` — position cache

### Common (header-only, C++11)
- `common/include/packets.h` — all packet structs (ConnectRequest, PlayerState, NPCSpawnRemote, etc.)
- `common/include/protocol.h` — constants (ports, rates, limits)
- `common/include/serialization.h` — pack/unpack helpers (bool return + out reference)

### Tools
- `tools/test_client.cpp` — dummy joiner for testing NPC sync pipeline

## RE_Kenshi Setup
- RE_Kenshi v0.3.1 installed via official installer (handles exe downgrade)
- `RE_Kenshi.dll` + `KenshiLib.dll` + `CompressToolsLib.dll` in Kenshi root
- `Plugin=RE_Kenshi` in Plugins_x64.cfg
- `mods/KenshiMP/KenshiMP.dll` + `kenshi-mp-server.exe` + `RE_Kenshi.json` + `KenshiMP.mod`
- `RE_Kenshi.json`: `{"Plugins": ["KenshiMP.dll"]}`
- `KenshiMP.mod`: 32-byte binary stub (type=16, version=1, all zeros)
