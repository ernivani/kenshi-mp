# V100 Game Thread Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recompile core DLL with VS2010 (v100) toolset so KenshiLib::AddHook works, enabling all game code to run on the game thread — fixing all threading crashes.

**Architecture:** Switch core DLL from VS2022/C++17/background-thread to VS2010/C++11/game-thread-hook. Revert all workarounds (dummy positions, disabled NPC spawning, SEH guards, headless UI). Server and injector stay on VS2022/C++17.

**Tech Stack:** VS2010 v100 x64 toolset, C++11, KenshiLib, ENet, Ogre3D, MyGUI, Boost 1.60

---

### Task 1: Make common headers C++11 compatible

**Files:**
- Modify: `common/include/serialization.h`
- Modify: `common/CMakeLists.txt`

The common headers are shared between core (C++11) and server (C++17). Replace C++17 features with C++11 equivalents.

- [ ] **Step 1: Replace std::optional and std::is_trivially_copyable_v in serialization.h**

Replace the entire contents of `common/include/serialization.h` with:

```cpp
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "packets.h"

namespace kmp {

// ---------------------------------------------------------------------------
// Serialization helpers for raw packet structs over ENet
// ---------------------------------------------------------------------------

/// Pack any packet struct into a byte buffer suitable for enet_packet_create.
template <typename T>
inline std::vector<uint8_t> pack(const T& packet) {
    std::vector<uint8_t> buf(sizeof(T));
    std::memcpy(buf.data(), &packet, sizeof(T));
    return buf;
}

/// Unpack a byte buffer into a packet struct.
/// Returns true and fills 'out' if successful, false if buffer too small.
template <typename T>
inline bool unpack(const uint8_t* data, size_t length, T& out) {
    if (length < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data, sizeof(T));
    return true;
}

/// Read just the header from raw data to determine packet type.
/// Returns true and fills 'out' if successful.
inline bool peek_header(const uint8_t* data, size_t length, PacketHeader& out) {
    return unpack<PacketHeader>(data, length, out);
}

/// Validate that a packet header has the expected protocol version.
inline bool validate_version(const PacketHeader& header) {
    return header.version == PROTOCOL_VERSION;
}

// ---------------------------------------------------------------------------
// Safe string copy into fixed-size char arrays (null-terminated)
// ---------------------------------------------------------------------------
template <size_t N>
inline void safe_strcpy(char (&dst)[N], const char* src) {
    std::strncpy(dst, src, N - 1);
    dst[N - 1] = '\0';
}

} // namespace kmp
```

Key changes: `unpack` now returns `bool` and takes an output reference (no `std::optional`). `peek_header` follows the same pattern. Removed `std::is_trivially_copyable_v` static_assert (v100 doesn't support it).

- [ ] **Step 2: Update common/CMakeLists.txt to use C++11**

Replace `cxx_std_17` with `cxx_std_11`:

```cmake
# C++11 for compatibility with core DLL (v100 toolset)
target_compile_features(kenshi-mp-common INTERFACE cxx_std_11)
```

- [ ] **Step 3: Commit**

```bash
git add common/include/serialization.h common/CMakeLists.txt
git commit -m "feat(common): make serialization C++11 compatible for v100 toolset"
```

---

### Task 2: Update all callers of unpack/peek_header

**Files:**
- Modify: `core/src/player_sync.cpp` (packet dispatch)
- Modify: `server/src/session.cpp` (packet dispatch)
- Modify: `server/src/main.cpp` (no changes needed — doesn't call unpack)

The `unpack` and `peek_header` signatures changed. Update all call sites.

- [ ] **Step 1: Update player_sync.cpp packet dispatch**

Replace the `on_packet_received` function in `core/src/player_sync.cpp` with:

```cpp
static void on_packet_received(const uint8_t* data, size_t length) {
    PacketHeader header;
    if (!peek_header(data, length, header)) return;
    if (!validate_version(header)) return;

    switch (header.type) {
    case PacketType::CONNECT_ACCEPT: {
        ConnectAccept pkt;
        if (unpack(data, length, pkt)) {
            client_set_local_id(pkt.player_id);
            ui_on_connect_accept(pkt.player_id);
        }
        break;
    }

    case PacketType::SPAWN_NPC: {
        SpawnNPC pkt;
        if (unpack(data, length, pkt)) {
            npc_manager_on_spawn(pkt);
        }
        break;
    }

    case PacketType::PLAYER_STATE: {
        PlayerState pkt;
        if (unpack(data, length, pkt)) {
            if (pkt.player_id != client_get_local_id()) {
                npc_manager_on_state(pkt);
            }
        }
        break;
    }

    case PacketType::PLAYER_DISCONNECT: {
        PlayerDisconnect pkt;
        if (unpack(data, length, pkt)) {
            npc_manager_on_disconnect(pkt.player_id);
        }
        break;
    }

    case PacketType::CHAT_MESSAGE: {
        ChatMessage pkt;
        if (unpack(data, length, pkt)) {
            ui_on_chat(pkt);
        }
        break;
    }

    case PacketType::PONG:
        break;

    default:
        break;
    }
}
```

- [ ] **Step 2: Update server/src/session.cpp packet dispatch**

In `session_on_packet`, replace calls like `auto req = unpack<ConnectRequest>(data, length); if (!req) return;` with `ConnectRequest req; if (!unpack(data, length, req)) return;`. Apply to all packet handlers:

- `handle_connect_request`: `ConnectRequest req; if (!unpack(data, length, req)) return;` then use `req.` instead of `req->`
- `handle_player_state`: `PlayerState state; if (!unpack(data, length, state)) return;` then use `state.` instead of `state->`
- `handle_chat_message`: `ChatMessage msg; if (!unpack(data, length, msg)) return;` then use `msg.` instead of `msg->`
- `handle_ping`: `PingPacket ping; if (!unpack(data, length, ping)) return;` then use `ping.` instead of `ping->`
- `session_on_packet`: `PacketHeader header; if (!peek_header(data, length, header)) return; if (!validate_version(header)) return;` then use `header.type` instead of `header->type`

- [ ] **Step 3: Commit**

```bash
git add core/src/player_sync.cpp server/src/session.cpp
git commit -m "refactor: update all unpack/peek_header callers for C++11 API"
```

---

### Task 3: Update core CMakeLists.txt for v100 toolset

**Files:**
- Modify: `core/CMakeLists.txt`

- [ ] **Step 1: Replace core/CMakeLists.txt**

Replace the entire contents with:

```cmake
# core/ — KenshiMP plugin DLL loaded by RE_Kenshi
#
# MUST be compiled with VS2010 (v100) x64 toolset for KenshiLib ABI compatibility.
# Pass -T v100 to cmake, or set CMAKE_GENERATOR_TOOLSET=v100.

add_library(KenshiMP SHARED
    src/plugin.cpp
    src/client.cpp
    src/game_hooks.cpp
    src/player_sync.cpp
    src/npc_manager.cpp
    src/ui.cpp
)

# C++11 — required by v100 toolset
set_target_properties(KenshiMP PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    PREFIX ""
    OUTPUT_NAME "KenshiMP"
)

# Common protocol headers
target_link_libraries(KenshiMP PRIVATE kenshi-mp-common)

# KenshiLib (game API + hooking)
if(KENSHILIB_DIR)
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DIR}/Include
        ${KENSHILIB_DIR}/Include/ogre
        ${KENSHILIB_DIR}/Include/mygui
    )
    target_link_directories(KenshiMP PRIVATE
        ${KENSHILIB_DIR}/Libraries
        ${KENSHILIB_DIR}/Libraries/ogre
        ${KENSHILIB_DIR}/Libraries/mygui
    )
    if(KENSHILIB_EXAMPLES_DEPS)
        target_link_directories(KenshiMP PRIVATE
            ${KENSHILIB_EXAMPLES_DEPS}/KenshiLib/Libraries
        )
    endif()
    target_link_libraries(KenshiMP PRIVATE KenshiLib OgreMain_x64 MyGUIEngine_x64)
endif()

# Boost
if(BOOST_ROOT)
    target_include_directories(KenshiMP PRIVATE ${BOOST_ROOT})
    target_link_directories(KenshiMP PRIVATE
        ${BOOST_ROOT}/stage/lib
    )
endif()

# ENet
if(ENET_DIR)
    target_include_directories(KenshiMP PRIVATE ${ENET_DIR}/include)
    target_link_directories(KenshiMP PRIVATE
        ${ENET_DIR}/lib
        ${ENET_DIR}/build/Release
    )
    target_link_libraries(KenshiMP PRIVATE enet)
endif()

# Windows libs
target_link_libraries(KenshiMP PRIVATE ws2_32 winmm)
```

Key changes: Removed all VS2022 compat defines. Switched to C++11. Removed KENSHILIB_DEPS_DIR block (using KENSHILIB_DIR + BOOST_ROOT instead). Simplified.

- [ ] **Step 2: Rebuild ENet with v100 toolset**

ENet was built with VS2022. We need to rebuild it with v100 since we're linking it into a v100 DLL:

```bash
cd deps/enet
rm -rf build
cmake -B build -T v100 -A x64
cmake --build build --config Release
```

- [ ] **Step 3: Commit**

```bash
git add core/CMakeLists.txt
git commit -m "feat(build): switch core DLL to v100 toolset with C++11"
```

---

### Task 4: Rewrite plugin.cpp — game thread hook via AddHook

**Files:**
- Modify: `core/src/plugin.cpp`

- [ ] **Step 1: Replace plugin.cpp with AddHook game loop approach**

Replace the entire contents of `core/src/plugin.cpp` with:

```cpp
// plugin.cpp — KenshiLib plugin entry point for KenshiMP
//
// RE_Kenshi mod loader calls startPlugin() when the mod is loaded.
// We hook the game's main loop via KenshiLib::AddHook to get
// per-frame callbacks on the game thread.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <core/Functions.h>
#include <OgreLogManager.h>

namespace kmp {
    void client_init();
    void client_shutdown();
    void player_sync_init();
    void player_sync_shutdown();
    void player_sync_tick(float dt);
    void npc_manager_init();
    void npc_manager_shutdown();
    void ui_init();
    void ui_shutdown();
}

// ---------------------------------------------------------------------------
// Game loop hook
// ---------------------------------------------------------------------------
static void (*s_original_main_loop)(GameWorld* world, float time) = NULL;
static bool s_subsystems_initialized = false;

static void hooked_main_loop(GameWorld* world, float time) {
    // Call original game logic first
    if (s_original_main_loop) {
        s_original_main_loop(world, time);
    }

    // Defer subsystem init to first hook call (MyGUI not ready during startPlugin)
    if (!s_subsystems_initialized) {
        s_subsystems_initialized = true;
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Initialising subsystems...");
        kmp::client_init();
        kmp::npc_manager_init();
        kmp::player_sync_init();
        kmp::ui_init();
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Subsystems ready");
    }

    // Run multiplayer sync on the game thread
    kmp::player_sync_tick(time);
}

// ---------------------------------------------------------------------------
// Plugin entry point — called by RE_Kenshi mod loader
// ---------------------------------------------------------------------------
__declspec(dllexport) void startPlugin() {
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loading...");

    KenshiLib::HookStatus status = KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        &hooked_main_loop,
        &s_original_main_loop
    );

    if (status != KenshiLib::SUCCESS) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] FATAL: Failed to hook game loop!"
        );
        return;
    }

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loaded OK (game loop hooked)");
}
```

Key changes: Uses `KenshiLib::AddHook` with the template overload (no reinterpret_cast needed). Defers init to first hook call. No background thread. Uses `NULL` instead of `nullptr` (v100 compat).

- [ ] **Step 2: Commit**

```bash
git add core/src/plugin.cpp
git commit -m "feat(core): rewrite plugin to use AddHook on game thread (v100)"
```

---

### Task 5: Rewrite game_hooks.cpp — direct KenshiLib calls, no SEH

**Files:**
- Modify: `core/src/game_hooks.cpp`

- [ ] **Step 1: Replace game_hooks.cpp**

Replace the entire contents with:

```cpp
// game_hooks.cpp — KenshiLib accessor wrappers
//
// All functions run on the game thread (via AddHook), so no
// thread-safety guards are needed.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectFactory.h>

namespace kmp {

Character* game_get_player_character() {
    if (!ou) return NULL;
    if (!ou->player) return NULL;

    const lektor<Character*>& chars = ou->player->getAllPlayerCharacters();
    if (chars.count <= 0) return NULL;

    return chars.stuff[0];
}

RootObjectFactory* game_get_factory() {
    if (!ou) return NULL;
    return ou->theFactory;
}

bool game_is_world_loaded() {
    if (!ou) return false;
    if (!ou->player) return false;

    const lektor<Character*>& chars = ou->player->getAllPlayerCharacters();
    return chars.count > 0;
}

GameWorld* game_get_world() {
    return ou;
}

} // namespace kmp
```

Key changes: Removed all `__try/__except` SEH guards. Removed `#include <Windows.h>`. Uses `NULL` instead of `nullptr`. Uses explicit `lektor<Character*>&` type instead of `auto`.

- [ ] **Step 2: Commit**

```bash
git add core/src/game_hooks.cpp
git commit -m "feat(core): clean game_hooks — direct KenshiLib calls, no SEH guards"
```

---

### Task 6: Rewrite player_sync.cpp — real state reading on game thread

**Files:**
- Modify: `core/src/player_sync.cpp`

- [ ] **Step 1: Replace player_sync.cpp**

Replace the entire contents with:

```cpp
// player_sync.cpp — Synchronize local player state with remote server
//
// Runs on the game thread via AddHook. Reads player state from KenshiLib,
// sends over ENet, dispatches incoming packets to subsystems.

#include <cmath>
#include <cstring>
#include <functional>

#include <kenshi/Character.h>
#include <OgreVector3.h>
#include <OgreLogManager.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

// External subsystems
extern void client_poll();
extern void client_send_unreliable(const uint8_t* data, size_t length);
extern void client_send_reliable(const uint8_t* data, size_t length);
extern bool client_is_connected();
extern uint32_t client_get_local_id();
extern void client_set_local_id(uint32_t id);
extern void client_set_packet_callback(std::function<void(const uint8_t*, size_t)> cb);

extern Character* game_get_player_character();
extern bool game_is_world_loaded();

extern void npc_manager_on_spawn(const SpawnNPC& pkt);
extern void npc_manager_on_state(const PlayerState& pkt);
extern void npc_manager_on_disconnect(uint32_t player_id);
extern void npc_manager_update(float dt);

extern void ui_on_chat(const ChatMessage& pkt);
extern void ui_on_connect_accept(uint32_t player_id);
extern void ui_check_hotkey();

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static PlayerState s_last_sent_state;
static float       s_send_timer = 0.0f;
static bool        s_initialized = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static float distance_sq(const PlayerState& a, const PlayerState& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx*dx + dy*dy + dz*dz;
}

static bool read_local_player_state(PlayerState& out) {
    Character* ch = game_get_player_character();
    if (!ch) return false;

    Ogre::Vector3 pos = ch->getPosition();
    out.x = pos.x;
    out.y = pos.y;
    out.z = pos.z;
    out.yaw = 0.0f;
    out.speed = ch->getMovementSpeed();
    out.animation_id = 0;
    out.player_id = client_get_local_id();

    return true;
}

// ---------------------------------------------------------------------------
// Packet dispatch
// ---------------------------------------------------------------------------
static void on_packet_received(const uint8_t* data, size_t length) {
    PacketHeader header;
    if (!peek_header(data, length, header)) return;
    if (!validate_version(header)) return;

    switch (header.type) {
    case PacketType::CONNECT_ACCEPT: {
        ConnectAccept pkt;
        if (unpack(data, length, pkt)) {
            client_set_local_id(pkt.player_id);
            ui_on_connect_accept(pkt.player_id);
        }
        break;
    }

    case PacketType::SPAWN_NPC: {
        SpawnNPC pkt;
        if (unpack(data, length, pkt)) {
            npc_manager_on_spawn(pkt);
        }
        break;
    }

    case PacketType::PLAYER_STATE: {
        PlayerState pkt;
        if (unpack(data, length, pkt)) {
            if (pkt.player_id != client_get_local_id()) {
                npc_manager_on_state(pkt);
            }
        }
        break;
    }

    case PacketType::PLAYER_DISCONNECT: {
        PlayerDisconnect pkt;
        if (unpack(data, length, pkt)) {
            npc_manager_on_disconnect(pkt.player_id);
        }
        break;
    }

    case PacketType::CHAT_MESSAGE: {
        ChatMessage pkt;
        if (unpack(data, length, pkt)) {
            ui_on_chat(pkt);
        }
        break;
    }

    case PacketType::PONG:
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void player_sync_init() {
    std::memset(&s_last_sent_state, 0, sizeof(s_last_sent_state));
    s_send_timer = 0.0f;
    client_set_packet_callback(on_packet_received);
    s_initialized = true;
}

void player_sync_shutdown() {
    s_initialized = false;
}

// ---------------------------------------------------------------------------
// Tick — called every frame on the game thread via AddHook
// ---------------------------------------------------------------------------
void player_sync_tick(float dt) {
    if (!s_initialized) return;

    // Always check for hotkeys
    ui_check_hotkey();

    // Poll network if connected
    if (client_is_connected()) {
        client_poll();
    }

    // Only do game sync when connected and world is loaded
    if (!client_is_connected() || !game_is_world_loaded()) return;

    // Update remote NPC positions (interpolation)
    npc_manager_update(dt);

    // Send local player state at tick rate
    s_send_timer += dt;
    if (s_send_timer >= TICK_INTERVAL_SEC) {
        s_send_timer = 0.0f;

        PlayerState current;
        if (read_local_player_state(current)) {
            if (distance_sq(current, s_last_sent_state) > POSITION_EPSILON * POSITION_EPSILON ||
                current.animation_id != s_last_sent_state.animation_id) {

                std::vector<uint8_t> buf = pack(current);
                client_send_unreliable(buf.data(), buf.size());
                s_last_sent_state = current;
            }
        }
    }
}

} // namespace kmp
```

Key changes: Real `ch->getPosition()` and `ch->getMovementSpeed()` calls. Re-enabled `game_is_world_loaded()` check. Removed debug tick logging. Removed dummy positions. Uses new `unpack`/`peek_header` API. Uses explicit types instead of `auto`.

- [ ] **Step 2: Commit**

```bash
git add core/src/player_sync.cpp
git commit -m "feat(core): restore real player state reading on game thread"
```

---

### Task 7: Rewrite npc_manager.cpp — real NPC spawning on game thread

**Files:**
- Modify: `core/src/npc_manager.cpp`

- [ ] **Step 1: Replace npc_manager.cpp**

Replace the entire contents with:

```cpp
// npc_manager.cpp — Manage NPCs representing remote players
//
// Runs on the game thread. Spawns/moves/despawns NPCs via KenshiLib.

#include <map>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/RootObjectFactory.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreMath.h>
#include <OgreLogManager.h>

#include "packets.h"
#include "protocol.h"

namespace kmp {

extern GameWorld* game_get_world();
extern RootObjectFactory* game_get_factory();

// ---------------------------------------------------------------------------
// High-resolution monotonic clock
// ---------------------------------------------------------------------------
static double get_time_sec() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

// ---------------------------------------------------------------------------
// Remote player state
// ---------------------------------------------------------------------------
struct RemotePlayer {
    uint32_t player_id;
    char     name[MAX_NAME_LENGTH];
    char     model[MAX_MODEL_LENGTH];
    Character* npc;

    struct Snapshot {
        float x, y, z;
        float yaw;
        uint32_t animation_id;
        float speed;
        double timestamp;
    };

    Snapshot prev;
    Snapshot next;
    double  interp_t;
};

static std::map<uint32_t, RemotePlayer> s_remote_players;

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void npc_manager_init() {
    s_remote_players.clear();
}

void npc_manager_shutdown() {
    GameWorld* world = game_get_world();
    for (std::map<uint32_t, RemotePlayer>::iterator it = s_remote_players.begin();
         it != s_remote_players.end(); ++it) {
        if (it->second.npc && world) {
            world->destroy(it->second.npc, false, "KenshiMP shutdown");
        }
    }
    s_remote_players.clear();
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
    Snapshot snap = { pkt.x, pkt.y, pkt.z, pkt.yaw, 0, 0.0f, now };
    rp.prev = snap;
    rp.next = snap;
    rp.interp_t = 1.0;

    // Spawn NPC via KenshiLib (safe — we're on the game thread)
    RootObjectFactory* factory = game_get_factory();
    if (factory) {
        Ogre::Vector3 spawn_pos(pkt.x, pkt.y, pkt.z);

        RootObjectBase* obj = factory->createRandomCharacter(
            NULL, spawn_pos, NULL, NULL, NULL, 0.0f
        );

        Character* npc = dynamic_cast<Character*>(obj);
        if (npc) {
            if (npc->ai) {
                npc->ai = NULL;
            }
            rp.npc = npc;
            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] Spawned NPC for player " + std::to_string(pkt.player_id));
        } else {
            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] WARNING: Failed to spawn NPC for player " +
                std::to_string(pkt.player_id));
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
    rp.prev = rp.next;
    Snapshot snap = { pkt.x, pkt.y, pkt.z, pkt.yaw, pkt.animation_id, pkt.speed, get_time_sec() };
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
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] Despawned NPC for player " + std::to_string(player_id));
    }

    s_remote_players.erase(it);
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
    for (std::map<uint32_t, RemotePlayer>::iterator it = s_remote_players.begin();
         it != s_remote_players.end(); ++it) {
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
            Ogre::Vector3 pos(ix, iy, iz);
            Ogre::Quaternion rot(Ogre::Radian(iyaw), Ogre::Vector3::UNIT_Y);
            rp.npc->teleport(pos, rot);
        }
    }
}

} // namespace kmp
```

Key changes: Re-enabled `factory->createRandomCharacter()`, `npc->teleport()`, `world->destroy()`. Uses explicit iterator types (C++11 compat). Uses `NULL` instead of `nullptr`. Named the Snapshot type for aggregate init.

- [ ] **Step 2: Commit**

```bash
git add core/src/npc_manager.cpp
git commit -m "feat(core): restore real NPC spawning and teleport on game thread"
```

---

### Task 8: Rewrite ui.cpp — real MyGUI widgets on game thread

**Files:**
- Modify: `core/src/ui.cpp`

- [ ] **Step 1: Replace ui.cpp — remove headless mode, enable MyGUI**

Remove the early return in `ui_init()` that skips widget creation. Keep the Kenshi skin names, the try/catch, and the F9 auto-connect (useful for testing). The key change: `ui_init` now runs on the game thread (via the deferred init in the hook), so MyGUI should be ready.

In `ui_init()`, remove these lines at the top:

```cpp
    // Skip MyGUI widget creation for now — use hotkey-based connect instead
    // MyGUI skin/timing issues cause crashes during early init
    s_ui_initialized = true;
    s_ui_visible = false;
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] UI initialized (headless mode)");
    return;
```

So `ui_init` starts with the `MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();` check directly.

Also replace `auto*` with explicit types for C++11 compat:
- `auto* host_label` → `MyGUI::TextBox* host_label`
- `auto* port_label` → `MyGUI::TextBox* port_label`
- `auto buf = pack(...)` → `std::vector<uint8_t> buf = pack(...)`
- `std::string host = s_host_input->getCaption()` → keep as-is (std::string is C++11)
- `std::string port_str = s_port_input->getCaption()` → keep as-is

- [ ] **Step 2: Commit**

```bash
git add core/src/ui.cpp
git commit -m "feat(core): enable MyGUI widgets on game thread, remove headless mode"
```

---

### Task 9: Build and test

**Files:**
- No code changes, just build verification

- [ ] **Step 1: Configure CMake with v100 toolset**

```bash
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
DEPS="C:/Users/tlind/Documents/code/kenshi-mp/deps"

cd C:/Users/tlind/Documents/code/kenshi-mp
rm -rf build

"$CMAKE" -B build -T v100 -A x64 \
  -DENET_DIR="$DEPS/enet" \
  -DKENSHILIB_DIR="$DEPS/KenshiLib" \
  -DKENSHILIB_EXAMPLES_DEPS="$DEPS/KenshiLib_Examples_deps" \
  -DBOOST_ROOT="$DEPS/KenshiLib_Examples_deps/boost_1_60_0" \
  -DKENSHIMP_BUILD_CORE=ON \
  -DKENSHIMP_BUILD_SERVER=OFF \
  -DKENSHIMP_BUILD_INJECTOR=OFF
```

Note: `-T v100` sets the toolset for the entire build. Since server/injector are OFF, only core uses v100. To build server/injector separately, run a second cmake configure without `-T v100`.

Expected: Configuration succeeds.

- [ ] **Step 2: Build core DLL**

```bash
cmake --build build --config Release --target KenshiMP
```

Expected: KenshiMP.dll built at `build/core/Release/KenshiMP.dll`

- [ ] **Step 3: Deploy and test**

```bash
cp build/core/Release/KenshiMP.dll "/c/Program Files (x86)/Steam/steamapps/common/Kenshi/mods/KenshiMP/"
```

Test sequence:
1. Start `kenshi-mp-server.exe` (build separately with VS2022 if needed)
2. Launch Kenshi
3. Check `kenshi.log` for `[KenshiMP] Plugin loaded OK (game loop hooked)`
4. Load a save
5. Check log for `[KenshiMP] Subsystems ready`
6. Press F8 — connect dialog should appear
7. Press F9 — auto-connect to localhost:7777
8. Server should log connection
9. Walk around — server should receive position updates
