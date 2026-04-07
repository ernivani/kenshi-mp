# KenshiMP Playable MVP — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the KenshiMP mod functional — players connect to a server, see each other as NPCs, move in real-time, and chat via an in-game overlay.

**Architecture:** KenshiLib plugin DLL loaded by RE_Kenshi mod loader. Hooks `GameWorld::mainLoop_GPUSensitiveStuff` for per-frame updates. Reads player position via KenshiLib API, sends over ENet UDP at 20Hz, spawns remote players as NPCs via `RootObjectFactory`. MyGUI overlay for connect dialog and chat. Standalone server (already working) relays packets.

**Tech Stack:** C++11 (core DLL, ABI compat with KenshiLib), C++17 (server/injector), KenshiLib, ENet, MyGUI, Ogre3D, spdlog, nlohmann/json

---

### Task 1: Rewrite `plugin.cpp` — KenshiLib entry point and game loop hook

**Files:**
- Modify: `core/src/plugin.cpp` (full rewrite)

This is the foundation — everything else depends on the game loop hook working.

- [ ] **Step 1: Replace plugin.cpp with KenshiLib entry point**

Replace the entire contents of `core/src/plugin.cpp` with:

```cpp
// plugin.cpp — KenshiLib plugin entry point for KenshiMP
//
// RE_Kenshi mod loader calls startPlugin() when the mod is loaded.
// We hook the game's main render loop to run our sync every frame.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <core/Functions.h>
#include <OgreLogManager.h>

// Forward declarations for our subsystems
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
// Original function pointer — filled by KenshiLib::AddHook
static void (*s_original_main_loop)(GameWorld* world, float time) = nullptr;

static void hooked_main_loop(GameWorld* world, float time) {
    // Run the original game logic first
    if (s_original_main_loop) {
        s_original_main_loop(world, time);
    }

    // Then run our multiplayer sync
    kmp::player_sync_tick(time);
}

// ---------------------------------------------------------------------------
// Plugin entry point — called by RE_Kenshi mod loader
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void startPlugin() {
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loading...");

    // Hook the game's main render loop
    // GameWorld::mainLoop_GPUSensitiveStuff is called every frame with delta time
    auto status = KenshiLib::AddHook(
        reinterpret_cast<void*>(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff)
        ),
        reinterpret_cast<void*>(&hooked_main_loop),
        reinterpret_cast<void**>(&s_original_main_loop)
    );

    if (status != HookStatus::SUCCESS) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] FATAL: Failed to hook game loop!"
        );
        return;
    }

    // Init subsystems
    kmp::client_init();
    kmp::npc_manager_init();
    kmp::player_sync_init();
    kmp::ui_init();

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loaded OK");
}
```

- [ ] **Step 2: Verify it compiles**

Build the core DLL target:
```bash
cmake --build build --target KenshiMP --config Release 2>&1 | head -30
```

Expected: Compiles without errors (assuming KenshiLib headers are available). Linker will find `KenshiLib::AddHook` and `KenshiLib::GetRealAddress` from KenshiLib.lib.

- [ ] **Step 3: Commit**

```bash
git add core/src/plugin.cpp
git commit -m "feat(core): rewrite plugin entry to use KenshiLib startPlugin + game loop hook"
```

---

### Task 2: Rewrite `game_hooks.cpp` — Real KenshiLib accessors

**Files:**
- Modify: `core/src/game_hooks.cpp` (full rewrite)

Replace placeholder accessors with real KenshiLib API calls.

- [ ] **Step 1: Replace game_hooks.cpp with KenshiLib accessors**

Replace the entire contents of `core/src/game_hooks.cpp` with:

```cpp
// game_hooks.cpp — KenshiLib accessor wrappers
//
// Provides game state access to other subsystems.
// All KenshiLib-specific code is isolated here.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectFactory.h>

namespace kmp {

// ---------------------------------------------------------------------------
// Get the local player's primary Character.
// Returns nullptr if the game world isn't loaded yet or player has no characters.
// ---------------------------------------------------------------------------
Character* game_get_player_character() {
    if (!ou) return nullptr;
    if (!ou->player) return nullptr;

    const auto& chars = ou->player->getAllPlayerCharacters();
    if (chars.count <= 0) return nullptr;

    return chars.stuff[0];
}

// ---------------------------------------------------------------------------
// Get the RootObjectFactory for spawning NPCs.
// ---------------------------------------------------------------------------
RootObjectFactory* game_get_factory() {
    if (!ou) return nullptr;
    return ou->theFactory;
}

// ---------------------------------------------------------------------------
// Check if the game world is fully loaded and the player has characters.
// ---------------------------------------------------------------------------
bool game_is_world_loaded() {
    if (!ou) return false;
    if (!ou->player) return false;

    const auto& chars = ou->player->getAllPlayerCharacters();
    return chars.count > 0;
}

// ---------------------------------------------------------------------------
// Get the GameWorld pointer (for destroy calls, etc.)
// ---------------------------------------------------------------------------
GameWorld* game_get_world() {
    return ou;
}

} // namespace kmp
```

- [ ] **Step 2: Commit**

```bash
git add core/src/game_hooks.cpp
git commit -m "feat(core): replace placeholder game_hooks with real KenshiLib accessors"
```

---

### Task 3: Fill in `player_sync.cpp` — Real state reading and delta time

**Files:**
- Modify: `core/src/player_sync.cpp`

Wire up real player state reading and accept real delta time from the hook.

- [ ] **Step 1: Rewrite player_sync.cpp with real KenshiLib state reading**

Replace the entire contents of `core/src/player_sync.cpp` with:

```cpp
// player_sync.cpp — Synchronize local player state with remote server
//
// Each frame (via game loop hook):
//   1. Poll network for incoming packets
//   2. Read local player position/rotation from KenshiLib
//   3. If changed significantly, send PLAYER_STATE to server (20Hz)
//   4. Update remote NPC interpolation

#include <cmath>
#include <cstring>
#include <functional>

#include <kenshi/Character.h>
#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <OgreMath.h>

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

// Read the local player's current state from KenshiLib
static bool read_local_player_state(PlayerState& out) {
    Character* ch = game_get_player_character();
    if (!ch) return false;

    Ogre::Vector3 pos = ch->getPosition();
    out.x = pos.x;
    out.y = pos.y;
    out.z = pos.z;

    // Extract yaw from the character's raw position data
    // KenshiLib Character inherits from RootObjectBase which has pos field
    // For rotation, we use the movement direction or fall back to 0
    out.yaw = 0.0f;
    float speed = ch->getMovementSpeed();
    out.speed = speed;

    out.animation_id = 0;  // MVP: idle only, animation sync is future work
    out.player_id = client_get_local_id();

    return true;
}

// ---------------------------------------------------------------------------
// Packet dispatch
// ---------------------------------------------------------------------------
static void on_packet_received(const uint8_t* data, size_t length) {
    auto header = peek_header(data, length);
    if (!header || !validate_version(*header)) return;

    switch (header->type) {
    case PacketType::CONNECT_ACCEPT: {
        auto pkt = unpack<ConnectAccept>(data, length);
        if (pkt) {
            client_set_local_id(pkt->player_id);
            ui_on_connect_accept(pkt->player_id);
        }
        break;
    }

    case PacketType::SPAWN_NPC: {
        auto pkt = unpack<SpawnNPC>(data, length);
        if (pkt) {
            npc_manager_on_spawn(*pkt);
        }
        break;
    }

    case PacketType::PLAYER_STATE: {
        auto pkt = unpack<PlayerState>(data, length);
        if (pkt && pkt->player_id != client_get_local_id()) {
            npc_manager_on_state(*pkt);
        }
        break;
    }

    case PacketType::PLAYER_DISCONNECT: {
        auto pkt = unpack<PlayerDisconnect>(data, length);
        if (pkt) {
            npc_manager_on_disconnect(pkt->player_id);
        }
        break;
    }

    case PacketType::CHAT_MESSAGE: {
        auto pkt = unpack<ChatMessage>(data, length);
        if (pkt) {
            ui_on_chat(*pkt);
        }
        break;
    }

    case PacketType::PONG:
        // TODO: calculate RTT for display
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
// Tick — called every frame from the game loop hook with real delta time
// ---------------------------------------------------------------------------
void player_sync_tick(float dt) {
    if (!s_initialized || !client_is_connected()) return;
    if (!game_is_world_loaded()) return;

    // Poll network
    client_poll();

    // Update remote NPC positions (interpolation)
    npc_manager_update(dt);

    // Send local player state at tick rate
    s_send_timer += dt;
    if (s_send_timer >= TICK_INTERVAL_SEC) {
        s_send_timer = 0.0f;

        PlayerState current;
        if (read_local_player_state(current)) {
            // Only send if position changed significantly
            if (distance_sq(current, s_last_sent_state) > POSITION_EPSILON * POSITION_EPSILON ||
                current.animation_id != s_last_sent_state.animation_id) {

                auto buf = pack(current);
                client_send_unreliable(buf.data(), buf.size());
                s_last_sent_state = current;
            }
        }
    }
}

} // namespace kmp
```

- [ ] **Step 2: Commit**

```bash
git add core/src/player_sync.cpp
git commit -m "feat(core): wire player_sync to real KenshiLib state reading and real delta time"
```

---

### Task 4: Fill in `npc_manager.cpp` — Real NPC spawning, movement, and despawning

**Files:**
- Modify: `core/src/npc_manager.cpp` (full rewrite)

This is the most complex task — spawning game NPCs, teleporting them, and cleaning them up.

- [ ] **Step 1: Rewrite npc_manager.cpp with real KenshiLib NPC operations**

Replace the entire contents of `core/src/npc_manager.cpp` with:

```cpp
// npc_manager.cpp — Manage NPCs representing remote players
//
// Each remote player is represented as a Kenshi NPC in the local game world.
// Spawned via KenshiLib's RootObjectFactory, moved via Character::teleport(),
// despawned via GameWorld::destroy().

#include <map>
#include <cstring>

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

// External
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
// Remote player state (for interpolation)
// ---------------------------------------------------------------------------
struct RemotePlayer {
    uint32_t player_id;
    char     name[MAX_NAME_LENGTH];
    char     model[MAX_MODEL_LENGTH];

    // NPC handle — KenshiLib Character object
    Character* npc;

    // Interpolation: two snapshots, blend between them
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

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static std::map<uint32_t, RemotePlayer> s_remote_players;

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void npc_manager_init() {
    s_remote_players.clear();
}

void npc_manager_shutdown() {
    GameWorld* world = game_get_world();
    for (auto& pair : s_remote_players) {
        if (pair.second.npc && world) {
            world->destroy(pair.second.npc, false, "KenshiMP shutdown");
        }
    }
    s_remote_players.clear();
}

// ---------------------------------------------------------------------------
// Spawn a new remote player NPC
// ---------------------------------------------------------------------------
void npc_manager_on_spawn(const SpawnNPC& pkt) {
    if (s_remote_players.count(pkt.player_id)) return;

    RemotePlayer rp;
    std::memset(&rp, 0, sizeof(rp));
    rp.player_id = pkt.player_id;
    std::strncpy(rp.name, pkt.name, MAX_NAME_LENGTH - 1);
    std::strncpy(rp.model, pkt.model, MAX_MODEL_LENGTH - 1);
    rp.npc = nullptr;

    double now = get_time_sec();
    rp.prev = { pkt.x, pkt.y, pkt.z, pkt.yaw, 0, 0.0f, now };
    rp.next = rp.prev;
    rp.interp_t = 1.0;

    // Spawn NPC via KenshiLib
    RootObjectFactory* factory = game_get_factory();
    if (factory) {
        Ogre::Vector3 spawn_pos(pkt.x, pkt.y, pkt.z);
        Ogre::Quaternion spawn_rot(Ogre::Radian(pkt.yaw), Ogre::Vector3::UNIT_Y);

        // Use createRandomCharacter to spawn a basic humanoid NPC
        // faction = nullptr (neutral), no home building, age 0
        RootObjectBase* obj = factory->createRandomCharacter(
            nullptr,          // faction (neutral)
            spawn_pos,        // position
            nullptr,          // container
            nullptr,          // character template (default)
            nullptr,          // home building
            0.0f              // age
        );

        Character* npc = dynamic_cast<Character*>(obj);
        if (npc) {
            // Disable AI so we control movement directly via teleport
            if (npc->ai) {
                npc->ai = nullptr;
            }
            rp.npc = npc;

            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] Spawned NPC for player " + std::to_string(pkt.player_id)
                + " '" + std::string(pkt.name) + "'"
            );
        } else {
            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] WARNING: Failed to spawn NPC for player "
                + std::to_string(pkt.player_id)
            );
        }
    }

    s_remote_players[pkt.player_id] = rp;
}

// ---------------------------------------------------------------------------
// Update position from network
// ---------------------------------------------------------------------------
void npc_manager_on_state(const PlayerState& pkt) {
    auto it = s_remote_players.find(pkt.player_id);
    if (it == s_remote_players.end()) return;

    RemotePlayer& rp = it->second;

    // Shift: current "next" becomes "prev", new data becomes "next"
    rp.prev = rp.next;
    rp.next = {
        pkt.x, pkt.y, pkt.z,
        pkt.yaw,
        pkt.animation_id,
        pkt.speed,
        get_time_sec()
    };
    rp.interp_t = 0.0;
}

// ---------------------------------------------------------------------------
// Remove a disconnected player's NPC
// ---------------------------------------------------------------------------
void npc_manager_on_disconnect(uint32_t player_id) {
    auto it = s_remote_players.find(player_id);
    if (it == s_remote_players.end()) return;

    if (it->second.npc) {
        GameWorld* world = game_get_world();
        if (world) {
            world->destroy(it->second.npc, false, "KenshiMP disconnect");
        }

        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] Despawned NPC for player " + std::to_string(player_id)
        );
    }

    s_remote_players.erase(it);
}

// ---------------------------------------------------------------------------
// Per-frame update — interpolate all remote player NPCs
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
    for (auto& pair : s_remote_players) {
        RemotePlayer& rp = pair.second;

        // Advance interpolation over one tick interval
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

- [ ] **Step 2: Commit**

```bash
git add core/src/npc_manager.cpp
git commit -m "feat(core): implement real NPC spawning, teleport movement, and despawning via KenshiLib"
```

---

### Task 5: Fill in `ui.cpp` — MyGUI connect dialog and chat window

**Files:**
- Modify: `core/src/ui.cpp` (full rewrite)

Create the actual MyGUI widgets for connecting to a server and chatting.

- [ ] **Step 1: Rewrite ui.cpp with real MyGUI widgets**

Replace the entire contents of `core/src/ui.cpp` with:

```cpp
// ui.cpp — MyGUI-based UI overlay for KenshiMP
//
// Kenshi uses MyGUI for its in-game UI. We access the same MyGUI instance
// to add our own windows: connect dialog, chat, and status bar.
//
// All MyGUI calls happen on the main/render thread (called from player_sync_tick
// via the game loop hook), so they are thread-safe.

#include <string>
#include <vector>
#include <cstring>

#include <MyGUI.h>
#include <OgreLogManager.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

// External
extern bool client_connect(const char* host, uint16_t port);
extern void client_disconnect();
extern bool client_is_connected();
extern uint32_t client_get_local_id();
extern void client_send_reliable(const uint8_t* data, size_t length);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_ui_initialized = false;
static bool s_ui_visible = false;

// MyGUI widgets
static MyGUI::Window*   s_connect_window = nullptr;
static MyGUI::EditBox*  s_host_input     = nullptr;
static MyGUI::EditBox*  s_port_input     = nullptr;
static MyGUI::Button*   s_connect_btn    = nullptr;
static MyGUI::Button*   s_disconnect_btn = nullptr;

static MyGUI::Window*   s_chat_window    = nullptr;
static MyGUI::EditBox*  s_chat_display   = nullptr;
static MyGUI::EditBox*  s_chat_input     = nullptr;
static MyGUI::Button*   s_chat_send_btn  = nullptr;

static MyGUI::TextBox*  s_status_text    = nullptr;

// Chat log
struct ChatEntry {
    std::string sender;
    std::string message;
};
static std::vector<ChatEntry> s_chat_log;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void on_connect_clicked(MyGUI::Widget* sender);
static void on_disconnect_clicked(MyGUI::Widget* sender);
static void on_chat_send_clicked(MyGUI::Widget* sender);
static void on_chat_key_press(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);
static void refresh_chat_display();
static void update_status_text();

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void ui_init() {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] WARNING: MyGUI not available, UI disabled"
        );
        return;
    }

    // --- Connect dialog ---
    s_connect_window = gui->createWidget<MyGUI::Window>(
        "WindowCSX",
        MyGUI::IntCoord(100, 100, 320, 200),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_ConnectWindow"
    );
    s_connect_window->setCaption("KenshiMP - Connect");
    s_connect_window->setVisible(false);

    // Host label + input
    auto* host_label = s_connect_window->createWidget<MyGUI::TextBox>(
        "TextBox",
        MyGUI::IntCoord(10, 10, 60, 26),
        MyGUI::Align::Default,
        "KMP_HostLabel"
    );
    host_label->setCaption("Host:");

    s_host_input = s_connect_window->createWidget<MyGUI::EditBox>(
        "EditBox",
        MyGUI::IntCoord(75, 10, 220, 26),
        MyGUI::Align::Default,
        "KMP_HostInput"
    );
    s_host_input->setCaption("127.0.0.1");

    // Port label + input
    auto* port_label = s_connect_window->createWidget<MyGUI::TextBox>(
        "TextBox",
        MyGUI::IntCoord(10, 44, 60, 26),
        MyGUI::Align::Default,
        "KMP_PortLabel"
    );
    port_label->setCaption("Port:");

    s_port_input = s_connect_window->createWidget<MyGUI::EditBox>(
        "EditBox",
        MyGUI::IntCoord(75, 44, 220, 26),
        MyGUI::Align::Default,
        "KMP_PortInput"
    );
    s_port_input->setCaption("7777");

    // Connect button
    s_connect_btn = s_connect_window->createWidget<MyGUI::Button>(
        "Button",
        MyGUI::IntCoord(10, 80, 140, 30),
        MyGUI::Align::Default,
        "KMP_ConnectBtn"
    );
    s_connect_btn->setCaption("Connect");
    s_connect_btn->eventMouseButtonClick += MyGUI::newDelegate(on_connect_clicked);

    // Disconnect button
    s_disconnect_btn = s_connect_window->createWidget<MyGUI::Button>(
        "Button",
        MyGUI::IntCoord(160, 80, 140, 30),
        MyGUI::Align::Default,
        "KMP_DisconnectBtn"
    );
    s_disconnect_btn->setCaption("Disconnect");
    s_disconnect_btn->eventMouseButtonClick += MyGUI::newDelegate(on_disconnect_clicked);

    // --- Chat window ---
    s_chat_window = gui->createWidget<MyGUI::Window>(
        "WindowCSX",
        MyGUI::IntCoord(100, 320, 400, 250),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_ChatWindow"
    );
    s_chat_window->setCaption("KenshiMP - Chat");
    s_chat_window->setVisible(false);

    // Chat display (read-only, multiline)
    s_chat_display = s_chat_window->createWidget<MyGUI::EditBox>(
        "EditBoxStretch",
        MyGUI::IntCoord(5, 5, 380, 170),
        MyGUI::Align::Stretch,
        "KMP_ChatDisplay"
    );
    s_chat_display->setEditReadOnly(true);
    s_chat_display->setEditMultiLine(true);
    s_chat_display->setEditWordWrap(true);

    // Chat input
    s_chat_input = s_chat_window->createWidget<MyGUI::EditBox>(
        "EditBox",
        MyGUI::IntCoord(5, 180, 310, 26),
        MyGUI::Align::Default,
        "KMP_ChatInput"
    );
    s_chat_input->eventKeyButtonPressed += MyGUI::newDelegate(on_chat_key_press);

    // Send button
    s_chat_send_btn = s_chat_window->createWidget<MyGUI::Button>(
        "Button",
        MyGUI::IntCoord(320, 180, 70, 26),
        MyGUI::Align::Default,
        "KMP_ChatSendBtn"
    );
    s_chat_send_btn->setCaption("Send");
    s_chat_send_btn->eventMouseButtonClick += MyGUI::newDelegate(on_chat_send_clicked);

    // --- Status text (top of screen) ---
    s_status_text = gui->createWidget<MyGUI::TextBox>(
        "TextBox",
        MyGUI::IntCoord(10, 5, 400, 24),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_StatusText"
    );
    s_status_text->setCaption("KenshiMP — Press F8 to open");
    s_status_text->setTextColour(MyGUI::Colour(0.8f, 1.0f, 0.8f));
    s_status_text->setVisible(true);

    s_ui_initialized = true;
    s_ui_visible = false;

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] UI initialized");
}

void ui_shutdown() {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (gui) {
        if (s_connect_window) { gui->destroyWidget(s_connect_window); s_connect_window = nullptr; }
        if (s_chat_window)    { gui->destroyWidget(s_chat_window);    s_chat_window = nullptr; }
        if (s_status_text)    { gui->destroyWidget(s_status_text);    s_status_text = nullptr; }
    }

    s_chat_log.clear();
    s_ui_initialized = false;
}

// ---------------------------------------------------------------------------
// Toggle visibility (called on F8 press)
// ---------------------------------------------------------------------------
void ui_toggle() {
    if (!s_ui_initialized) return;

    s_ui_visible = !s_ui_visible;
    if (s_connect_window) s_connect_window->setVisible(s_ui_visible);
    if (s_chat_window && client_is_connected()) s_chat_window->setVisible(s_ui_visible);
}

// ---------------------------------------------------------------------------
// Check for F8 key press — call this from player_sync_tick or game hook
// ---------------------------------------------------------------------------
void ui_check_hotkey() {
    // Check F8 key state via Windows API (works regardless of game input state)
    static bool s_f8_was_down = false;
    bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;

    if (f8_down && !s_f8_was_down) {
        ui_toggle();
    }
    s_f8_was_down = f8_down;
}

// ---------------------------------------------------------------------------
// Callbacks from player_sync
// ---------------------------------------------------------------------------
void ui_on_connect_accept(uint32_t player_id) {
    ChatEntry entry;
    entry.sender = "[KenshiMP]";
    entry.message = "Connected! Your ID: " + std::to_string(player_id);
    s_chat_log.push_back(entry);
    refresh_chat_display();
    update_status_text();

    // Show chat window
    if (s_chat_window && s_ui_visible) {
        s_chat_window->setVisible(true);
    }
}

void ui_on_chat(const ChatMessage& pkt) {
    ChatEntry entry;
    entry.sender = "Player " + std::to_string(pkt.player_id);
    entry.message = pkt.message;
    s_chat_log.push_back(entry);
    refresh_chat_display();
}

// ---------------------------------------------------------------------------
// UI actions
// ---------------------------------------------------------------------------
void ui_send_chat(const char* message) {
    if (!client_is_connected()) return;
    if (!message || message[0] == '\0') return;

    ChatMessage pkt;
    pkt.player_id = client_get_local_id();
    safe_strcpy(pkt.message, message);

    auto buf = pack(pkt);
    client_send_reliable(buf.data(), buf.size());
}

// ---------------------------------------------------------------------------
// Widget callbacks
// ---------------------------------------------------------------------------
static void on_connect_clicked(MyGUI::Widget* sender) {
    if (client_is_connected()) return;
    if (!s_host_input || !s_port_input) return;

    std::string host = s_host_input->getCaption();
    std::string port_str = s_port_input->getCaption();
    uint16_t port = static_cast<uint16_t>(std::stoi(port_str));

    // Send connect request after ENet connection
    if (client_connect(host.c_str(), port)) {
        ConnectRequest req;
        safe_strcpy(req.name, "Player");
        safe_strcpy(req.model, "greenlander");
        auto buf = pack(req);
        client_send_reliable(buf.data(), buf.size());

        update_status_text();
    }
}

static void on_disconnect_clicked(MyGUI::Widget* sender) {
    if (!client_is_connected()) return;
    client_disconnect();
    update_status_text();

    if (s_chat_window) s_chat_window->setVisible(false);
}

static void on_chat_send_clicked(MyGUI::Widget* sender) {
    if (!s_chat_input) return;

    std::string msg = s_chat_input->getCaption();
    if (msg.empty()) return;

    ui_send_chat(msg.c_str());
    s_chat_input->setCaption("");
}

static void on_chat_key_press(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch) {
    if (key == MyGUI::KeyCode::Return) {
        on_chat_send_clicked(sender);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void refresh_chat_display() {
    if (!s_chat_display) return;

    std::string text;
    // Show last 50 messages
    size_t start = s_chat_log.size() > 50 ? s_chat_log.size() - 50 : 0;
    for (size_t i = start; i < s_chat_log.size(); ++i) {
        text += s_chat_log[i].sender + ": " + s_chat_log[i].message + "\n";
    }

    s_chat_display->setCaption(text);
}

static void update_status_text() {
    if (!s_status_text) return;

    if (client_is_connected()) {
        s_status_text->setCaption(
            "KenshiMP — Connected as Player #" + std::to_string(client_get_local_id())
        );
        s_status_text->setTextColour(MyGUI::Colour(0.4f, 1.0f, 0.4f));
    } else {
        s_status_text->setCaption("KenshiMP — Disconnected (F8 to open)");
        s_status_text->setTextColour(MyGUI::Colour(1.0f, 0.6f, 0.6f));
    }
}

} // namespace kmp
```

- [ ] **Step 2: Add ui_check_hotkey call to player_sync_tick**

In `core/src/player_sync.cpp`, add the hotkey check. Add the extern declaration near the top with the other externs:

```cpp
extern void ui_check_hotkey();
```

Then add this call at the very beginning of `player_sync_tick`, **before** the `if (!s_initialized || !client_is_connected())` check, so the F8 key works even when disconnected:

```cpp
void player_sync_tick(float dt) {
    ui_check_hotkey();

    if (!s_initialized || !client_is_connected()) return;
    if (!game_is_world_loaded()) return;
    // ... rest unchanged
```

- [ ] **Step 3: Commit**

```bash
git add core/src/ui.cpp core/src/player_sync.cpp
git commit -m "feat(core): implement MyGUI connect dialog, chat window, and status overlay"
```

---

### Task 6: Rewrite `injector/src/main.cpp` — Mod folder deployment

**Files:**
- Modify: `injector/src/main.cpp` (full rewrite)

Switch from Plugins_x64.cfg patching to copying DLL into mods folder.

- [ ] **Step 1: Rewrite injector for mod folder deployment**

Replace the entire contents of `injector/src/main.cpp` with:

```cpp
// main.cpp — KenshiMP Launcher
//
// Deploys KenshiMP.dll to Kenshi's mods folder and launches the game.
// Requires RE_Kenshi (KenshiLib mod loader) to be installed.
//
// Flow:
//   1. Find Kenshi install (Steam registry or CLI arg)
//   2. Check RE_Kenshi is installed
//   3. Create mods/KenshiMP/ folder
//   4. Copy KenshiMP.dll into it
//   5. Launch kenshi_x64.exe
//   6. Wait for exit

#include <iostream>
#include <string>
#include <filesystem>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fs = std::filesystem;

static const char* DLL_NAME = "KenshiMP.dll";
static const char* MOD_FOLDER = "mods";
static const char* MOD_NAME = "KenshiMP";

// ---------------------------------------------------------------------------
// Find Kenshi install via Steam registry
// ---------------------------------------------------------------------------
static std::string find_kenshi_steam() {
    HKEY key;
    const char* reg_path = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 233860";

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, reg_path, 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        char buffer[MAX_PATH];
        DWORD size = sizeof(buffer);
        if (RegQueryValueExA(key, "InstallLocation", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return std::string(buffer);
        }
        RegCloseKey(key);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Check if RE_Kenshi (mod loader) is installed
// ---------------------------------------------------------------------------
static bool check_re_kenshi(const fs::path& kenshi_dir) {
    // RE_Kenshi installs as a DLL in the Kenshi directory
    // Common names: RE_Kenshi.dll, or it patches the exe
    // Check for the mods folder existing as a basic indicator
    auto mods_dir = kenshi_dir / MOD_FOLDER;
    if (!fs::exists(mods_dir)) {
        std::cout << "Note: '" << MOD_FOLDER << "' folder not found. Creating it.\n";
        std::cout << "Make sure RE_Kenshi (KenshiLib mod loader) is installed!\n";
        std::cout << "Without it, KenshiMP.dll will not be loaded.\n\n";
    }
    return true;
}

// ---------------------------------------------------------------------------
// Deploy DLL to mods folder
// ---------------------------------------------------------------------------
static bool deploy_dll(const fs::path& kenshi_dir) {
    auto mod_dir = kenshi_dir / MOD_FOLDER / MOD_NAME;
    auto dst = mod_dir / DLL_NAME;

    // Create mod directory
    std::error_code ec;
    fs::create_directories(mod_dir, ec);
    if (ec) {
        std::cerr << "Error creating mod directory: " << ec.message() << "\n";
        return false;
    }

    // Find the DLL (next to launcher, or in parent, or in build output)
    fs::path src;
    fs::path candidates[] = {
        fs::path(DLL_NAME),
        fs::path("..") / DLL_NAME,
        fs::path("..") / "core" / "Release" / DLL_NAME,
        fs::path("Release") / DLL_NAME,
    };

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            src = candidate;
            break;
        }
    }

    if (src.empty()) {
        if (fs::exists(dst)) {
            std::cout << "Using existing " << DLL_NAME << " in mod folder\n";
            return true;
        }
        std::cerr << "Error: " << DLL_NAME << " not found.\n";
        std::cerr << "Place it next to this launcher or build it first.\n";
        return false;
    }

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Error copying DLL: " << ec.message() << "\n";
        return false;
    }

    std::cout << "Deployed " << DLL_NAME << " to " << mod_dir << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// Launch Kenshi
// ---------------------------------------------------------------------------
static int launch_kenshi(const fs::path& kenshi_dir) {
    auto exe = kenshi_dir / "kenshi_x64.exe";
    if (!fs::exists(exe)) {
        std::cerr << "Error: " << exe << " not found\n";
        return 1;
    }

    std::cout << "Launching " << exe << "...\n";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::string exe_str = exe.string();
    std::string dir_str = kenshi_dir.string();

    if (!CreateProcessA(
            exe_str.c_str(),
            nullptr,
            nullptr, nullptr,
            FALSE, 0,
            nullptr,
            dir_str.c_str(),
            &si, &pi)) {
        std::cerr << "Error: CreateProcess failed (" << GetLastError() << ")\n";
        return 1;
    }

    std::cout << "Kenshi started (PID: " << pi.dwProcessId << ")\n";
    std::cout << "Waiting for Kenshi to exit...\n";

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::cout << "Kenshi exited with code " << exit_code << "\n";
    return static_cast<int>(exit_code);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::cout << "=== KenshiMP Launcher ===\n\n";

    // Determine Kenshi directory
    std::string kenshi_path;
    if (argc > 1) {
        kenshi_path = argv[1];
    } else {
        kenshi_path = find_kenshi_steam();
        if (kenshi_path.empty()) {
            std::cerr << "Could not find Kenshi install directory.\n";
            std::cerr << "Usage: " << argv[0] << " <path-to-kenshi>\n";
            return 1;
        }
    }

    fs::path kenshi_dir(kenshi_path);
    std::cout << "Kenshi directory: " << kenshi_dir << "\n";

    if (!fs::exists(kenshi_dir / "kenshi_x64.exe")) {
        std::cerr << "Error: kenshi_x64.exe not found in " << kenshi_dir << "\n";
        return 1;
    }

    // Check for mod loader
    check_re_kenshi(kenshi_dir);

    // Deploy DLL
    if (!deploy_dll(kenshi_dir)) return 1;

    // Launch
    return launch_kenshi(kenshi_dir);
}
```

- [ ] **Step 2: Commit**

```bash
git add injector/src/main.cpp
git commit -m "feat(injector): rewrite to deploy DLL to mods folder instead of patching Plugins_x64.cfg"
```

---

### Task 7: Update `core/CMakeLists.txt` — KenshiLib linking

**Files:**
- Modify: `core/CMakeLists.txt`

Ensure KenshiLib headers and library are properly linked, including MinHook for `AddHook`.

- [ ] **Step 1: Replace core/CMakeLists.txt**

Replace the entire contents of `core/CMakeLists.txt` with:

```cmake
# core/ — KenshiMP plugin DLL loaded by RE_Kenshi
#
# Build requirements:
#   - Visual Studio 2019+ with Windows 7.1 SDK toolset
#   - KenshiLib (headers + lib)
#   - KenshiLib_Examples_deps (Ogre, Boost, MinHook, MyGUI)
#   - ENet

add_library(KenshiMP SHARED
    src/plugin.cpp
    src/client.cpp
    src/game_hooks.cpp
    src/player_sync.cpp
    src/npc_manager.cpp
    src/ui.cpp
)

target_compile_features(KenshiMP PRIVATE cxx_std_11)

# Common protocol headers
target_link_libraries(KenshiMP PRIVATE kenshi-mp-common)

# KenshiLib (game API + hooking)
if(KENSHILIB_DIR)
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DIR}/include
    )
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE KenshiLib)
endif()

# KenshiLib deps root (contains Ogre, Boost, MinHook, MyGUI)
if(KENSHILIB_DEPS_DIR)
    # Ogre3D
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DEPS_DIR}/ogre/OgreMain/include
        ${KENSHILIB_DEPS_DIR}/ogre/build/include
    )
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/ogre/build/lib/Release)
    target_link_libraries(KenshiMP PRIVATE OgreMain)

    # Boost
    target_include_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/boost_1_60_0)
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/boost_1_60_0/lib64-msvc-10.0)

    # MyGUI
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DEPS_DIR}/mygui/MyGUIEngine/include
        ${KENSHILIB_DEPS_DIR}/mygui/build/include
    )
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/mygui/build/lib/Release)
    target_link_libraries(KenshiMP PRIVATE MyGUIEngine)

    # MinHook (used by KenshiLib::AddHook)
    target_include_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/minhook/include)
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/minhook/build/lib/Release)
    target_link_libraries(KenshiMP PRIVATE MinHook)
else()
    # Fallback: use individual dir variables
    if(OGRE_DIR)
        target_include_directories(KenshiMP PRIVATE ${OGRE_DIR}/include/OGRE)
        target_link_directories(KenshiMP PRIVATE ${OGRE_DIR}/lib)
        target_link_libraries(KenshiMP PRIVATE OgreMain)
    endif()

    if(BOOST_ROOT)
        target_include_directories(KenshiMP PRIVATE ${BOOST_ROOT})
        target_link_directories(KenshiMP PRIVATE ${BOOST_ROOT}/lib64-msvc-10.0)
    endif()

    if(MYGUI_DIR)
        target_include_directories(KenshiMP PRIVATE ${MYGUI_DIR}/include)
        target_link_directories(KenshiMP PRIVATE ${MYGUI_DIR}/lib)
        target_link_libraries(KenshiMP PRIVATE MyGUIEngine)
    endif()
endif()

# ENet
if(ENET_DIR)
    target_include_directories(KenshiMP PRIVATE ${ENET_DIR}/include)
    target_link_directories(KenshiMP PRIVATE ${ENET_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE enet)
endif()

# Windows libs (networking for ENet, input for GetAsyncKeyState)
target_link_libraries(KenshiMP PRIVATE ws2_32 winmm)

# Output: KenshiMP.dll (no "lib" prefix)
set_target_properties(KenshiMP PROPERTIES
    PREFIX ""
    OUTPUT_NAME "KenshiMP"
)
```

- [ ] **Step 2: Add KENSHILIB_DEPS_DIR option to root CMakeLists.txt**

In `CMakeLists.txt` (root), add this line after the existing `ENET_DIR` cache variable:

```cmake
set(KENSHILIB_DEPS_DIR "" CACHE PATH "Path to KenshiLib_Examples_deps root")
```

- [ ] **Step 3: Commit**

```bash
git add core/CMakeLists.txt CMakeLists.txt
git commit -m "feat(build): update CMake to support KENSHILIB_DEPS_DIR and link MinHook for AddHook"
```

---

### Task 8: Wire up `ui_check_hotkey` in `player_sync_tick` and ensure full call flow

**Files:**
- Modify: `core/src/player_sync.cpp` (small edit)

The `player_sync_tick` function needs to call `ui_check_hotkey()` even when not connected, and it also needs to handle the case where the game world is loaded but we're not connected (so the user can open the UI and connect).

- [ ] **Step 1: Update player_sync_tick to always check hotkey and poll when connected**

In `core/src/player_sync.cpp`, the `player_sync_tick` function should be updated. Replace the function body with:

```cpp
void player_sync_tick(float dt) {
    if (!s_initialized) return;

    // Always check for F8 hotkey, even when disconnected
    ui_check_hotkey();

    // Only do network + sync when connected and world is loaded
    if (!client_is_connected()) return;
    if (!game_is_world_loaded()) return;

    // Poll network
    client_poll();

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

                auto buf = pack(current);
                client_send_unreliable(buf.data(), buf.size());
                s_last_sent_state = current;
            }
        }
    }
}
```

Note: We also need to poll the network even when the world isn't loaded (to receive CONNECT_ACCEPT). Update the flow:

```cpp
void player_sync_tick(float dt) {
    if (!s_initialized) return;

    // Always check for F8 hotkey
    ui_check_hotkey();

    // Poll network if connected (to receive CONNECT_ACCEPT, etc.)
    if (client_is_connected()) {
        client_poll();
    }

    // Only do game sync when world is loaded
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

                auto buf = pack(current);
                client_send_unreliable(buf.data(), buf.size());
                s_last_sent_state = current;
            }
        }
    }
}
```

- [ ] **Step 2: Commit**

```bash
git add core/src/player_sync.cpp
git commit -m "fix(core): ensure hotkey and network poll work before world is loaded"
```

---

### Task 9: Final integration check — verify all externs and linkage

**Files:**
- Review: all `core/src/*.cpp` files

This task verifies that all `extern` declarations match actual function signatures across files, and that the build configuration is consistent.

- [ ] **Step 1: Verify extern consistency**

Check that these function signatures match across files:

| Function | Declared in | Defined in |
|----------|-------------|------------|
| `client_init()` | plugin.cpp | client.cpp |
| `client_shutdown()` | plugin.cpp | client.cpp |
| `client_poll()` | player_sync.cpp | client.cpp |
| `client_send_unreliable(...)` | player_sync.cpp | client.cpp |
| `client_send_reliable(...)` | player_sync.cpp, ui.cpp | client.cpp |
| `client_is_connected()` | player_sync.cpp, ui.cpp | client.cpp |
| `client_get_local_id()` | player_sync.cpp, ui.cpp | client.cpp |
| `client_set_local_id(...)` | player_sync.cpp | client.cpp |
| `client_set_packet_callback(...)` | player_sync.cpp | client.cpp |
| `client_connect(...)` | ui.cpp | client.cpp |
| `client_disconnect()` | ui.cpp | client.cpp |
| `game_get_player_character()` | player_sync.cpp | game_hooks.cpp |
| `game_is_world_loaded()` | player_sync.cpp | game_hooks.cpp |
| `game_get_factory()` | npc_manager.cpp | game_hooks.cpp |
| `game_get_world()` | npc_manager.cpp | game_hooks.cpp |
| `player_sync_init()` | plugin.cpp | player_sync.cpp |
| `player_sync_shutdown()` | plugin.cpp | player_sync.cpp |
| `player_sync_tick(float)` | plugin.cpp | player_sync.cpp |
| `npc_manager_init()` | plugin.cpp | npc_manager.cpp |
| `npc_manager_shutdown()` | plugin.cpp | npc_manager.cpp |
| `npc_manager_on_spawn(...)` | player_sync.cpp | npc_manager.cpp |
| `npc_manager_on_state(...)` | player_sync.cpp | npc_manager.cpp |
| `npc_manager_on_disconnect(...)` | player_sync.cpp | npc_manager.cpp |
| `npc_manager_update(float)` | player_sync.cpp | npc_manager.cpp |
| `ui_init()` | plugin.cpp | ui.cpp |
| `ui_shutdown()` | plugin.cpp | ui.cpp |
| `ui_on_chat(...)` | player_sync.cpp | ui.cpp |
| `ui_on_connect_accept(...)` | player_sync.cpp | ui.cpp |
| `ui_check_hotkey()` | player_sync.cpp | ui.cpp |

Key changes from the old code:
- `game_get_player_character()` now returns `Character*` (was `void*`)
- `game_get_factory()` now returns `RootObjectFactory*` (was `void*`)
- `game_get_world()` is new
- `player_sync_tick(float dt)` now takes a parameter (was `void`)
- `ui_check_hotkey()` is new
- `game_hooks_install()` / `game_hooks_remove()` are removed (hooking moved to plugin.cpp)

Update the extern declarations in `player_sync.cpp` — the `game_get_player_character` return type changed from `void*` to `Character*`. This was already done in the Task 3 rewrite. Verify the forward declaration in `plugin.cpp` uses `void player_sync_tick(float dt)` not `void player_sync_tick()`.

- [ ] **Step 2: Verify the `client.cpp` function that uses `std::function` compiles with C++11**

The `client_set_packet_callback` uses `std::function<void(const uint8_t*, size_t)>`. This is valid C++11. However, the `common/` headers use `std::optional` and `std::is_trivially_copyable_v` which are C++17. Since `client.cpp` includes common headers, and the core target is set to `cxx_std_11`, this will fail.

Fix: Change `core/CMakeLists.txt` to use C++17:

```cmake
target_compile_features(KenshiMP PRIVATE cxx_std_17)
```

KenshiLib requires ABI compatibility at the binary level (matching calling convention, struct layout), not necessarily the same C++ standard. C++17 with the Windows7.1SDK toolset should work.

- [ ] **Step 3: Commit**

```bash
git add core/CMakeLists.txt
git commit -m "fix(build): use C++17 for core DLL (required by common/ headers using std::optional)"
```

---

### Task 10: Verify complete build

**Files:**
- No changes, just verification

- [ ] **Step 1: Configure CMake with all dependencies**

```bash
cmake -B build -A x64 \
  -DKENSHILIB_DIR=<path-to-KenshiLib> \
  -DKENSHILIB_DEPS_DIR=<path-to-KenshiLib_Examples_deps> \
  -DENET_DIR=<path-to-enet> \
  -DKENSHIMP_BUILD_CORE=ON \
  -DKENSHIMP_BUILD_SERVER=ON \
  -DKENSHIMP_BUILD_INJECTOR=ON
```

Expected: Configuration succeeds, finds all headers and libraries.

- [ ] **Step 2: Build all targets**

```bash
cmake --build build --config Release
```

Expected: All three targets compile and link:
- `build/core/Release/KenshiMP.dll`
- `build/server/Release/kenshi-mp-server.exe`
- `build/injector/Release/kenshi-mp-launcher.exe`

- [ ] **Step 3: Smoke test server**

```bash
./build/server/Release/kenshi-mp-server.exe
```

Expected output:
```
[info] KenshiMP Dedicated Server v0.1.0
[info] No config file found at 'server_config.json', using defaults
[info] Port: 7777, Max players: 32, Name: KenshiMP Server
[info] World state initialized
[info] Server listening on port 7777
```

- [ ] **Step 4: Smoke test launcher**

```bash
./build/injector/Release/kenshi-mp-launcher.exe
```

Expected: Finds Kenshi install, creates mods/KenshiMP/ folder, copies DLL, launches game.

- [ ] **Step 5: In-game test**

1. Start server
2. Launch Kenshi via launcher
3. In game, press F8 — connect dialog should appear
4. Enter localhost:7777, click Connect
5. Server should log the connection
6. Status text should update to "Connected as Player #1"
7. Open a second Kenshi client, connect — both should see NPCs for each other
8. Type a chat message — should appear on both clients
9. Disconnect one client — NPC should despawn on the other
