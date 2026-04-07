# KenshiMP Playable MVP — Design Spec

## Goal

Make the existing KenshiMP codebase functional: two+ Kenshi clients connect to a dedicated server, see each other as NPCs moving in real-time, and can chat via an in-game UI overlay.

## Scope

- Position/rotation sync between players (20Hz)
- Remote players rendered as NPCs with interpolation
- MyGUI connect dialog (host/port) and chat window
- Server relay (already working, no changes)
- Protocol/serialization (already working, no changes)

**Out of scope:** combat sync, inventory, animations beyond idle, quest sync, auth/anti-cheat, world persistence.

## Architecture (unchanged)

```
Game Process (kenshi_x64.exe)
  └─ RE_Kenshi loads KenshiMP.dll from mods/KenshiMP/
       ├─ KenshiLib API → reads player position, spawns NPCs
       ├─ ENet UDP client → sends/receives state at 20Hz
       └─ MyGUI overlay → connect dialog, chat, status
                ↕ (ENet UDP)
         Dedicated Server (standalone .exe)
           ├─ Relays player state between clients
           ├─ Session management, timeouts, chat
           └─ No changes needed
```

## Changes by File

### 1. `core/src/plugin.cpp` — Rewrite

**Current:** Exports `dllStartPlugin`/`dllStopPlugin` (Ogre plugin style). Wrong for KenshiLib.

**New:** Export `startPlugin()` called by RE_Kenshi mod loader.

- Remove `OgrePlugin.h` includes and `KenshiMPPlugin` class
- Export `void __declspec(dllexport) startPlugin()`
- In `startPlugin()`:
  1. Hook `GameWorld::_NV_mainLoop_GPUSensitiveStuff` via `KenshiLib::AddHook`
  2. Call `client_init()`, `npc_manager_init()`, `player_sync_init()`, `ui_init()`
- The hook function:
  1. Calls original game update
  2. Calls `player_sync_tick(dt)` with the real delta time from the hook parameter

No `stopPlugin` needed — KenshiLib plugins don't have a formal shutdown. Use `atexit()` or DLL_PROCESS_DETACH if cleanup is needed.

### 2. `core/src/game_hooks.cpp` — Fill in KenshiLib accessors

**Current:** All functions return nullptr/false.

**New:**
- `game_get_player_character()`: Return `ou->player->getAllPlayerCharacters()` first character, or `ou->player->selectedCharacter.getCharacter()`. Returns `Character*`.
- `game_get_factory()`: Return `ou->theFactory`.
- `game_is_world_loaded()`: Check `ou != nullptr && ou->player != nullptr && ou->player->getAllPlayerCharacters().count > 0`.
- Remove `game_hooks_install()`/`game_hooks_remove()` — hooking moves to `plugin.cpp`.
- Include KenshiLib headers: `kenshi/Globals.h`, `kenshi/GameWorld.h`, `kenshi/Character.h`, `kenshi/PlayerInterface.h`.

### 3. `core/src/player_sync.cpp` — Fill in state reading

**Current:** `read_local_player_state()` never reads real data. `player_sync_tick()` uses fake dt.

**New:**
- `read_local_player_state()`:
  - Cast `game_get_player_character()` to `Character*`
  - `pos = character->getPosition()` → fill `out.x/y/z`
  - Extract yaw from character orientation → fill `out.yaw`
  - `out.speed = character->getMovementSpeed()`
  - `out.animation_id = 0` (MVP: idle only)
  - `out.player_id = client_get_local_id()`
- `player_sync_tick(float dt)`: Accept `dt` as parameter instead of using `TICK_INTERVAL_SEC` constant. The hook passes real frame delta.
- Update send timer to use real dt.

### 4. `core/src/npc_manager.cpp` — Fill in NPC spawning/movement

**Current:** `npc_handle` always nullptr, `get_time_sec()` returns 0.

**New:**

**Clock:** Implement `get_time_sec()` using `QueryPerformanceCounter`/`QueryPerformanceFrequency` for accurate monotonic timing.

**Spawn (`npc_manager_on_spawn`):**
- Get factory: `RootObjectFactory* factory = ou->theFactory`
- Find a character template from game data (e.g., "greenlander" male) via `ou->gamedata`
- Spawn: `factory->createRandomCharacter(faction, position, container, template, nullptr, 0.0f)`
- Store returned `Character*` as `npc_handle`
- Disable AI so we control movement directly
- Add to game world update list

**Move (`npc_manager_update`):**
- For each remote player with valid `npc_handle`:
  - Interpolate position/yaw as before (lerp between prev/next snapshots)
  - Apply via `character->teleport(Ogre::Vector3(ix, iy, iz), quaternion_from_yaw(iyaw))`

**Despawn (`npc_manager_on_disconnect`):**
- `ou->destroy(rootObject, false, "KenshiMP disconnect")`
- Remove from map

### 5. `core/src/ui.cpp` — Fill in MyGUI widgets

**Current:** All widget creation commented out.

**New:**

**Connect dialog (shown on F8):**
- `MyGUI::Gui::getInstancePtr()` to access GUI
- Create Window "KenshiMP - Connect" (300x180)
- EditBox for host (default "127.0.0.1")
- EditBox for port (default "7777")
- Button "Connect" → calls `client_connect(host, port)` then sends `ConnectRequest`
- Button "Disconnect" → calls `client_disconnect()`

**Chat window (shown after connect):**
- EditBox (read-only, multiline) for chat history
- EditBox for input
- Button "Send" or Enter key → calls `ui_send_chat()`

**Status text:**
- TextBox at top of screen: "KenshiMP — Connected as Player #N" or "Disconnected"

**Key binding:**
- Hook keyboard for F8 → `ui_toggle()`
- Hook Enter key when chat input focused → send chat

**Thread safety:** All MyGUI calls happen in `player_sync_tick()` which runs on the game's main/render thread via the hook. This is safe.

### 6. `injector/src/main.cpp` — Rework for mod folder

**Current:** Patches `Plugins_x64.cfg`, restores on exit.

**New:**
1. Find Kenshi install via Steam registry (keep)
2. Create `[Kenshi]/mods/KenshiMP/` directory
3. Copy `KenshiMP.dll` into that folder
4. Launch `kenshi_x64.exe`
5. No cleanup needed (mod folder can persist)
6. Remove all `Plugins_x64.cfg` backup/restore logic

Note: User still needs RE_Kenshi (the KenshiLib mod loader) installed for the mod to load. The injector should check for this and warn if not found.

### 7. `core/CMakeLists.txt` — Update linking

- Add KenshiLib headers and lib (already partially there)
- Add MinHook (bundled with KenshiLib deps) for `KenshiLib::AddHook`
- Keep ENet linking
- Keep MyGUI linking
- Keep `ws2_32`, `winmm`
- Toolset: document that VS2019+ with Windows7.1SDK platformtoolset is required

### 8. No changes

- `common/` — protocol, packets, serialization all stay as-is
- `server/` — relay, sessions, world_state all stay as-is
- Root `CMakeLists.txt` — no changes

## Data Flow (MVP)

```
1. Player A starts Kenshi with KenshiMP mod loaded
2. startPlugin() hooks game loop, inits subsystems
3. Player presses F8 → connect dialog appears
4. Player enters server IP:port, clicks Connect
5. ENet connects to server → server sends CONNECT_ACCEPT with player_id
6. Every frame (via hook):
   a. Poll network for incoming packets
   b. Read local player position from KenshiLib
   c. If moved significantly, send PLAYER_STATE to server (20Hz)
   d. Server relays to other clients
   e. Other clients receive PLAYER_STATE → update interpolation snapshots
   f. Interpolate remote NPCs → teleport to interpolated position
7. On new player join: server sends SPAWN_NPC → client spawns NPC
8. On disconnect: server sends PLAYER_DISCONNECT → client despawns NPC
9. Chat: type in chat input → CHAT_MESSAGE to server → broadcast to all
```

## Testing Plan

1. **Build server** with ENet — verify it starts, listens on 7777, logs connections
2. **Build core DLL** with KenshiLib — verify it loads in Kenshi, logs "[KenshiMP] Initialised"
3. **Test hook** — verify `player_sync_tick` is called each frame (log frame count)
4. **Test state reading** — log local player position each tick
5. **Test connect** — press F8, enter localhost:7777, verify server sees connection
6. **Test NPC spawn** — connect second client, verify NPC appears at correct position
7. **Test movement** — move player A, verify NPC moves on player B's screen
8. **Test chat** — send message from A, verify it appears on B
9. **Test disconnect** — disconnect A, verify NPC despawns on B

## Dependencies

| Dependency | Version | Purpose |
|-----------|---------|---------|
| KenshiLib | latest | Game API (hooking, characters, factory) |
| KenshiLib_Examples_deps | latest | Ogre, Boost 1.60, MinHook |
| ENet | 1.3.x | UDP networking |
| MyGUI | (from Kenshi) | In-game UI |
| nlohmann/json | 3.11.3 | Server config (FetchContent) |
| spdlog | 1.13.0 | Server logging (FetchContent) |

## Risks

- **KenshiLib version compatibility**: API may have changed since research. May need to adjust header paths or function signatures.
- **NPC template selection**: Need to find the right GameData template for spawning humanoid NPCs. May require trial and error.
- **AI disabling**: Need to confirm the right way to prevent spawned NPCs from running their own AI.
- **MyGUI skin names**: Widget skin/type names depend on Kenshi's MyGUI resource files. May need adjustment.
