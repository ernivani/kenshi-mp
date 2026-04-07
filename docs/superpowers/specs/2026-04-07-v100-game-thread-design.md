# KenshiMP v100 Game Thread Architecture — Design Spec

## Problem

The core DLL was compiled with VS2022 (v143), causing:
1. `KenshiLib::AddHook` fails — member function pointer ABI mismatch with VS2010-compiled KenshiLib
2. Background thread workaround crashes — KenshiLib/Ogre objects are not thread-safe
3. Ogre::FrameListener only fires once — Kenshi's game loop bypasses Ogre's frame callbacks

## Solution

Compile the core DLL with the **VS2010 (v100) x64 toolset**. This matches KenshiLib's ABI, enabling `AddHook` to hook the game loop. All code runs on the game thread — no threading issues.

## Changes

### 1. Build System

- Add `-T v100` (or `CMAKE_GENERATOR_TOOLSET=v100`) for the core DLL target
- Change core DLL from `cxx_std_17` to `cxx_std_11`
- Remove VS2022 compat defines: `_HAS_AUTO_PTR_ETC`, `_HAS_TR1_NAMESPACE`, `BOOST_ALL_NO_LIB`, `BOOST_ERROR_CODE_HEADER_ONLY`, `BOOST_SYSTEM_NO_DEPRECATED`
- Remove `UNICODE`/`_UNICODE` (KenshiLib PerfTimer.h issue was VS2022-specific)
- Link Boost natively (v100 static libs match now): `libboost_system-vc100-mt-1_60`, `libboost_thread-vc100-mt-1_60`
- Server and injector remain C++17 with VS2022 toolset — only core DLL uses v100

### 2. Common Headers (C++11 compat)

`common/include/serialization.h`:
- Replace `std::optional<T>` return types with raw pointer/bool patterns or a simple `Optional<T>` struct
- Replace `std::is_trivially_copyable_v<T>` with `std::is_trivially_copyable<T>::value`
- These changes must not break server/injector (C++17) — use `#if __cplusplus >= 201703L` guards or just use C++11 everywhere (simpler)

`common/include/protocol.h`:
- Already C++11 compatible, no changes needed

`common/include/packets.h`:
- Already C++11 compatible, no changes needed

### 3. Plugin Entry (`plugin.cpp`)

Revert to the AddHook approach:
- `startPlugin()` hooks `GameWorld::_NV_mainLoop_GPUSensitiveStuff` via `KenshiLib::AddHook`
- Hook calls original, then calls `player_sync_tick(dt)` on the game thread
- Defer subsystem init to first hook call (MyGUI not ready during `startPlugin`)
- Delete background thread, `CreateThread`, `Sleep`, all threading code

### 4. Game Hooks (`game_hooks.cpp`)

Revert to direct KenshiLib calls:
- `game_get_player_character()` → `ou->player->getAllPlayerCharacters().stuff[0]`
- `game_get_factory()` → `ou->theFactory`
- `game_is_world_loaded()` → check `ou->player` and character count
- `game_get_world()` → return `ou`
- Remove all `__try/__except` SEH guards — not needed on game thread
- Remove `#include <Windows.h>` — not needed

### 5. Player Sync (`player_sync.cpp`)

Revert to real state reading:
- `read_local_player_state()` → `ch->getPosition()`, `ch->getMovementSpeed()`
- Re-enable `game_is_world_loaded()` check
- Remove dummy position data
- Remove packet debug logging (keep for development, remove later)

### 6. NPC Manager (`npc_manager.cpp`)

Re-enable all KenshiLib game calls:
- `npc_manager_on_spawn()` → `factory->createRandomCharacter()`, disable AI, store Character*
- `npc_manager_update()` → `character->teleport(pos, rot)` for interpolation
- `npc_manager_on_disconnect()` → `world->destroy()` for despawning
- `npc_manager_shutdown()` → destroy all NPCs

### 7. UI (`ui.cpp`)

Re-enable MyGUI widget creation:
- Use Kenshi skin names: `Kenshi_GenericWindowSkin`, `Kenshi_TextboxStandardText`, `Kenshi_EditBoxStandardText`, `Kenshi_Button1Skin`
- Connect dialog, chat window, status text
- F8 hotkey via `GetAsyncKeyState`
- Remove headless mode / F9 auto-connect (replaced by real UI)
- Keep try/catch around widget creation as safety net

### 8. No Changes

- `server/` — already working, stays C++17/VS2022
- `injector/` — already working, stays C++17/VS2022
- `client.cpp` — logic unchanged, just recompiles with v100
- Protocol/packet format — unchanged

## User Action Required

Install the VS2010 x64 compiler toolset. Options:
1. Install Visual Studio 2010 (from archive.org) and its SP1
2. Or install just the Windows SDK 7.1 which includes the v100 compiler

The v100 toolset must be available at `C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\` for CMake to find it with `-T v100`.

## Testing

1. Build core DLL with `-T v100` — verify it compiles and links
2. Deploy to `mods/KenshiMP/` — verify RE_Kenshi loads it
3. Verify `[KenshiMP] Plugin loaded OK` in kenshi.log
4. Load a save — verify game loop hook fires (tick logging)
5. Press F8 — verify connect dialog appears
6. Connect to server — verify server logs connection
7. Walk around — verify server receives position updates
8. Second client — verify NPC spawns and moves
