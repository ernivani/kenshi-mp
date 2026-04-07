# Phase 1: Host Model + NPC Sync — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Joining players see the host's NPCs moving in real-time, spawned/despawned based on proximity to any player.

**Architecture:** Host client scans KenshiLib's character update list each tick, tracks which NPCs are in range of any player, sends spawn/state/despawn packets to the server which relays to joiners. Joiners create puppet NPCs that mirror host state. Uses a host-assigned sequential ID system to map KenshiLib `hand` handles to compact uint32 IDs.

**Tech Stack:** C++11 (v100 toolset), KenshiLib, ENet, Ogre3D. All core DLL code runs on the game thread.

**Important:** All core DLL code must be v100-compatible: no `auto`, no `nullptr` (use `NULL`), no `constexpr`, no `enum class`, no `std::function`, use `std::ostringstream` for int-to-string, explicit iterator types.

---

### Task 1: Add new packet types and protocol constants

**Files:**
- Modify: `common/include/protocol.h`
- Modify: `common/include/packets.h`

- [ ] **Step 1: Add NPC sync constants to protocol.h**

Add these lines after `MAX_CHAT_LENGTH` in `common/include/protocol.h`:

```cpp
static const float    NPC_SYNC_RADIUS     = 500.0f;
static const float    NPC_SYNC_INTERVAL   = 1.0f / 10; // 10Hz batch updates
static const uint16_t MAX_NPC_BATCH       = 49;         // per packet (MTU limit)
static const size_t   MAX_RACE_LENGTH     = 32;
static const size_t   MAX_WEAPON_LENGTH   = 64;
static const size_t   MAX_ARMOUR_LENGTH   = 64;
```

- [ ] **Step 2: Add new packet type constants to packets.h**

Add these after `PONG` in the `PacketType` namespace:

```cpp
    static const uint8_t NPC_BATCH_STATE    = 0x40;
    static const uint8_t NPC_SPAWN_REMOTE   = 0x41;
    static const uint8_t NPC_DESPAWN_REMOTE = 0x42;
```

- [ ] **Step 3: Add is_host field to ConnectRequest**

In `packets.h`, add `uint8_t is_host;` after the `model` field in `ConnectRequest`:

```cpp
struct ConnectRequest {
    PacketHeader header;
    char         name[MAX_NAME_LENGTH];
    char         model[MAX_MODEL_LENGTH];
    uint8_t      is_host;              // 1 = this player is the host

    ConnectRequest() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CONNECT_REQUEST;
    }
};
```

- [ ] **Step 4: Add NPC packet structs**

Add these structs before `#pragma pack(pop)` in `packets.h`:

```cpp
struct NPCSpawnRemote {
    PacketHeader header;
    uint32_t     npc_id;
    char         name[MAX_NAME_LENGTH];
    char         race[MAX_RACE_LENGTH];
    char         weapon[MAX_WEAPON_LENGTH];
    char         armour[MAX_ARMOUR_LENGTH];
    float        x, y, z;
    float        yaw;

    NPCSpawnRemote() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::NPC_SPAWN_REMOTE;
    }
};

struct NPCDespawnRemote {
    PacketHeader header;
    uint32_t     npc_id;

    NPCDespawnRemote() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::NPC_DESPAWN_REMOTE;
    }
};

// Single NPC state entry within a batch (no header — packed into NPCBatchState)
struct NPCStateEntry {
    uint32_t npc_id;
    float    x, y, z;
    float    yaw;
    float    speed;
    uint32_t animation_id;
};

// Variable-length packet: header + count + count*NPCStateEntry
// Do NOT use pack() — manually serialize. Use unpack helpers below.
struct NPCBatchHeader {
    PacketHeader header;
    uint16_t     count;

    NPCBatchHeader() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::NPC_BATCH_STATE;
    }
};
```

- [ ] **Step 5: Commit**

```bash
git add common/include/protocol.h common/include/packets.h
git commit -m "feat(proto): add NPC sync packet types, is_host flag, sync constants"
```

---

### Task 2: Update server to track host and relay NPC packets

**Files:**
- Modify: `server/src/session.cpp`

- [ ] **Step 1: Add is_host tracking to session**

Add `bool is_host;` to the `PlayerSession` struct and add a static `s_host_id`:

After `static uint32_t s_next_id = 1;` add:
```cpp
static uint32_t s_host_id = 0;  // player ID of the host (0 = no host)
```

In `PlayerSession` struct, add:
```cpp
    bool        is_host;
```

- [ ] **Step 2: Update handle_connect_request to read is_host**

After creating the session, read the flag:
```cpp
    session.is_host = (req.is_host != 0);
    if (session.is_host && s_host_id == 0) {
        s_host_id = id;
        spdlog::info("Player {} is the HOST", id);
    }
```

- [ ] **Step 3: Clear host on disconnect**

In `session_on_disconnect`, after getting the id, add:
```cpp
    if (s_host_id == id) {
        s_host_id = 0;
        spdlog::info("Host player disconnected");
    }
```

- [ ] **Step 4: Add NPC packet relay handler**

Add a new function:

```cpp
static void handle_npc_packet(ENetPeer* peer, const uint8_t* data, size_t length, bool reliable) {
    auto it = s_peer_to_id.find(peer);
    if (it == s_peer_to_id.end()) return;

    uint32_t id = it->second;

    // Only accept NPC packets from the host
    if (id != s_host_id) {
        spdlog::warn("Non-host player {} tried to send NPC packet, ignoring", id);
        return;
    }

    // Update activity timestamp
    s_sessions[id].last_activity = std::chrono::steady_clock::now();

    // Relay to all non-host players
    relay_broadcast(peer, data, length, reliable);
}
```

- [ ] **Step 5: Add NPC packet types to session_on_packet switch**

Add these cases before the `default:` case:

```cpp
    case PacketType::NPC_SPAWN_REMOTE:
        handle_npc_packet(peer, data, length, true);
        break;
    case PacketType::NPC_BATCH_STATE:
        handle_npc_packet(peer, data, length, false);
        break;
    case PacketType::NPC_DESPAWN_REMOTE:
        handle_npc_packet(peer, data, length, true);
        break;
```

- [ ] **Step 6: Commit**

```bash
git add server/src/session.cpp
git commit -m "feat(server): track host player, relay NPC packets to joiners"
```

---

### Task 3: Create host_sync.cpp — NPC scanning and sending

**Files:**
- Create: `core/src/host_sync.cpp`
- Modify: `core/CMakeLists.txt`

This is the core of the feature. The host scans all NPCs each tick, tracks which are in sync range, and sends spawn/state/despawn packets.

- [ ] **Step 1: Create core/src/host_sync.cpp**

```cpp
// host_sync.cpp — Host-side NPC state scanning and sync
//
// Runs on the game thread. Scans KenshiLib's character update list,
// filters by proximity to connected players, sends NPC spawn/state/despawn
// packets to the server for relay to joiners.

#include <map>
#include <vector>
#include <cstring>
#include <string>
#include <sstream>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/PlayerInterface.h>
#include <OgreVector3.h>
#include <OgreLogManager.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

extern void client_send_unreliable(const uint8_t* data, size_t length);
extern void client_send_reliable(const uint8_t* data, size_t length);
extern Character* game_get_player_character();

// v100-safe int to string
static std::string itos(uint32_t val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

// ---------------------------------------------------------------------------
// NPC ID system: map KenshiLib hand → compact sequential uint32_t
// ---------------------------------------------------------------------------
struct SyncedNPC {
    uint32_t npc_id;
    float    last_x, last_y, last_z;
};

static std::map<uint64_t, SyncedNPC> s_synced_npcs;  // hand_key → SyncedNPC
static uint32_t s_next_npc_id = 1;
static bool     s_is_host = false;
static float    s_npc_send_timer = 0.0f;

// Create a unique key from a Character pointer (stable within a session)
static uint64_t make_npc_key(Character* ch) {
    return (uint64_t)(uintptr_t)ch;
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void host_sync_init() {
    s_synced_npcs.clear();
    s_next_npc_id = 1;
    s_npc_send_timer = 0.0f;
}

void host_sync_shutdown() {
    s_synced_npcs.clear();
}

void host_sync_set_host(bool is_host) {
    s_is_host = is_host;
    if (is_host) {
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] This client is the HOST");
    }
}

bool host_sync_is_host() {
    return s_is_host;
}

// ---------------------------------------------------------------------------
// Check if an NPC is within sync radius of any player
// ---------------------------------------------------------------------------
static bool is_in_range_of_any_player(const Ogre::Vector3& npc_pos) {
    if (!ou || !ou->player) return false;

    const lektor<Character*>& players = ou->player->getAllPlayerCharacters();
    for (int i = 0; i < players.count; ++i) {
        Character* pc = players.stuff[i];
        if (!pc) continue;
        Ogre::Vector3 player_pos = pc->getPosition();
        float dx = npc_pos.x - player_pos.x;
        float dy = npc_pos.y - player_pos.y;
        float dz = npc_pos.z - player_pos.z;
        float dist_sq = dx*dx + dy*dy + dz*dz;
        if (dist_sq <= NPC_SYNC_RADIUS * NPC_SYNC_RADIUS) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Read NPC appearance data for spawn packet
// ---------------------------------------------------------------------------
static void fill_spawn_packet(NPCSpawnRemote& pkt, Character* ch, uint32_t npc_id) {
    pkt.npc_id = npc_id;

    // Name: use the game data string ID as a fallback name
    if (ch->data) {
        std::strncpy(pkt.name, ch->data->stringID.c_str(), MAX_NAME_LENGTH - 1);
        pkt.name[MAX_NAME_LENGTH - 1] = '\0';
    }

    // Race
    RaceData* race = ch->getRace();
    if (race && race->data) {
        std::strncpy(pkt.race, race->data->stringID.c_str(), MAX_RACE_LENGTH - 1);
        pkt.race[MAX_RACE_LENGTH - 1] = '\0';
    }

    // Position
    Ogre::Vector3 pos = ch->getPosition();
    pkt.x = pos.x;
    pkt.y = pos.y;
    pkt.z = pos.z;
    pkt.yaw = 0.0f;

    // Weapon and armour left empty for now — future improvement
    pkt.weapon[0] = '\0';
    pkt.armour[0] = '\0';
}

// ---------------------------------------------------------------------------
// Host sync tick — called every frame from player_sync_tick
// ---------------------------------------------------------------------------
void host_sync_tick(float dt) {
    if (!s_is_host) return;
    if (!ou) return;

    // Get all active characters
    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();

    // Build list of NPCs currently in range
    std::vector<Character*> in_range;

    ogre_unordered_set<Character*>::type::const_iterator it;
    for (it = chars.begin(); it != chars.end(); ++it) {
        Character* ch = *it;
        if (!ch) continue;
        if (ch->isPlayerCharacter()) continue;  // skip player characters

        Ogre::Vector3 pos = ch->getPosition();
        if (is_in_range_of_any_player(pos)) {
            in_range.push_back(ch);
        }
    }

    // Check for new NPCs entering range → send NPC_SPAWN_REMOTE
    for (size_t i = 0; i < in_range.size(); ++i) {
        Character* ch = in_range[i];
        uint64_t key = make_npc_key(ch);

        if (s_synced_npcs.find(key) == s_synced_npcs.end()) {
            // New NPC — assign ID and send spawn
            uint32_t npc_id = s_next_npc_id++;

            SyncedNPC snpc;
            snpc.npc_id = npc_id;
            Ogre::Vector3 pos = ch->getPosition();
            snpc.last_x = pos.x;
            snpc.last_y = pos.y;
            snpc.last_z = pos.z;
            s_synced_npcs[key] = snpc;

            NPCSpawnRemote spawn;
            fill_spawn_packet(spawn, ch, npc_id);
            std::vector<uint8_t> buf = pack(spawn);
            client_send_reliable(buf.data(), buf.size());
        }
    }

    // Check for NPCs that left range → send NPC_DESPAWN_REMOTE
    // Build set of in-range keys for fast lookup
    std::map<uint64_t, bool> in_range_keys;
    for (size_t i = 0; i < in_range.size(); ++i) {
        in_range_keys[make_npc_key(in_range[i])] = true;
    }

    std::vector<uint64_t> to_remove;
    std::map<uint64_t, SyncedNPC>::iterator synced_it;
    for (synced_it = s_synced_npcs.begin(); synced_it != s_synced_npcs.end(); ++synced_it) {
        if (in_range_keys.find(synced_it->first) == in_range_keys.end()) {
            // NPC left range — despawn
            NPCDespawnRemote despawn;
            despawn.npc_id = synced_it->second.npc_id;
            std::vector<uint8_t> buf = pack(despawn);
            client_send_reliable(buf.data(), buf.size());
            to_remove.push_back(synced_it->first);
        }
    }
    for (size_t i = 0; i < to_remove.size(); ++i) {
        s_synced_npcs.erase(to_remove[i]);
    }

    // Send batch position updates at NPC_SYNC_INTERVAL
    s_npc_send_timer += dt;
    if (s_npc_send_timer < NPC_SYNC_INTERVAL) return;
    s_npc_send_timer = 0.0f;

    if (in_range.empty()) return;

    // Build batch packets (max MAX_NPC_BATCH entries per packet)
    std::vector<uint8_t> batch_buf;
    uint16_t count = 0;

    // Reserve space for header
    NPCBatchHeader batch_hdr;
    batch_buf.resize(sizeof(NPCBatchHeader));

    for (size_t i = 0; i < in_range.size(); ++i) {
        Character* ch = in_range[i];
        uint64_t key = make_npc_key(ch);
        std::map<uint64_t, SyncedNPC>::iterator found = s_synced_npcs.find(key);
        if (found == s_synced_npcs.end()) continue;

        Ogre::Vector3 pos = ch->getPosition();

        NPCStateEntry entry;
        entry.npc_id = found->second.npc_id;
        entry.x = pos.x;
        entry.y = pos.y;
        entry.z = pos.z;
        entry.yaw = 0.0f;
        entry.speed = ch->getMovementSpeed();
        entry.animation_id = 0;

        // Append entry to buffer
        size_t offset = batch_buf.size();
        batch_buf.resize(offset + sizeof(NPCStateEntry));
        std::memcpy(&batch_buf[offset], &entry, sizeof(NPCStateEntry));
        count++;

        // Flush if we hit the batch limit
        if (count >= MAX_NPC_BATCH) {
            batch_hdr.count = count;
            std::memcpy(&batch_buf[0], &batch_hdr, sizeof(NPCBatchHeader));
            client_send_unreliable(batch_buf.data(), batch_buf.size());

            // Reset for next batch
            batch_buf.resize(sizeof(NPCBatchHeader));
            count = 0;
        }
    }

    // Send remaining entries
    if (count > 0) {
        batch_hdr.count = count;
        std::memcpy(&batch_buf[0], &batch_hdr, sizeof(NPCBatchHeader));
        client_send_unreliable(batch_buf.data(), batch_buf.size());
    }
}

} // namespace kmp
```

- [ ] **Step 2: Add host_sync.cpp to CMakeLists**

In `core/CMakeLists.txt`, add `src/host_sync.cpp` to the source list:

```cmake
add_library(KenshiMP SHARED
    src/plugin.cpp
    src/client.cpp
    src/game_hooks.cpp
    src/player_sync.cpp
    src/npc_manager.cpp
    src/host_sync.cpp
    src/ui.cpp
)
```

- [ ] **Step 3: Commit**

```bash
git add core/src/host_sync.cpp core/CMakeLists.txt
git commit -m "feat(core): add host_sync — NPC scanning, ID assignment, batch sending"
```

---

### Task 4: Extend npc_manager.cpp to handle remote NPC packets

**Files:**
- Modify: `core/src/npc_manager.cpp`

Add handlers for NPC_SPAWN_REMOTE, NPC_BATCH_STATE, NPC_DESPAWN_REMOTE. These use the same interpolation system as player NPCs but keyed by `npc_id`.

- [ ] **Step 1: Add remote NPC data structures and handlers**

Add a new map and handler functions after the existing `s_remote_players` section. Add these after `npc_manager_on_disconnect`:

```cpp
// ---------------------------------------------------------------------------
// Remote NPC sync (from host via server)
// ---------------------------------------------------------------------------
struct RemoteNPC {
    uint32_t   npc_id;
    Character* npc;
    Snapshot   prev;
    Snapshot   next;
    double     interp_t;
};

static std::map<uint32_t, RemoteNPC> s_remote_npcs;

void npc_manager_on_remote_spawn(const NPCSpawnRemote& pkt) {
    if (s_remote_npcs.count(pkt.npc_id)) return;

    RemoteNPC rnpc;
    std::memset(&rnpc, 0, sizeof(rnpc));
    rnpc.npc_id = pkt.npc_id;
    rnpc.npc = NULL;

    double now = get_time_sec();
    Snapshot snap;
    snap.x = pkt.x;
    snap.y = pkt.y;
    snap.z = pkt.z;
    snap.yaw = pkt.yaw;
    snap.animation_id = 0;
    snap.speed = 0.0f;
    snap.timestamp = now;
    rnpc.prev = snap;
    rnpc.next = snap;
    rnpc.interp_t = 1.0;

    // Spawn NPC via KenshiLib
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
            rnpc.npc = npc;
            Ogre::LogManager::getSingleton().logMessage(
                "[KenshiMP] Spawned remote NPC " + itos(pkt.npc_id) +
                " '" + std::string(pkt.name) + "' race=" + std::string(pkt.race));
        }
    }

    s_remote_npcs[pkt.npc_id] = rnpc;
}

void npc_manager_on_remote_state(const NPCStateEntry& entry) {
    std::map<uint32_t, RemoteNPC>::iterator it = s_remote_npcs.find(entry.npc_id);
    if (it == s_remote_npcs.end()) return;

    RemoteNPC& rnpc = it->second;
    rnpc.prev = rnpc.next;

    Snapshot snap;
    snap.x = entry.x;
    snap.y = entry.y;
    snap.z = entry.z;
    snap.yaw = entry.yaw;
    snap.animation_id = entry.animation_id;
    snap.speed = entry.speed;
    snap.timestamp = get_time_sec();
    rnpc.next = snap;
    rnpc.interp_t = 0.0;
}

void npc_manager_on_remote_despawn(uint32_t npc_id) {
    std::map<uint32_t, RemoteNPC>::iterator it = s_remote_npcs.find(npc_id);
    if (it == s_remote_npcs.end()) return;

    if (it->second.npc) {
        GameWorld* world = game_get_world();
        if (world) {
            world->destroy(it->second.npc, false, "KenshiMP NPC despawn");
        }
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] Despawned remote NPC " + itos(npc_id));
    }

    s_remote_npcs.erase(it);
}
```

- [ ] **Step 2: Update npc_manager_update to interpolate remote NPCs too**

At the end of `npc_manager_update`, add a second loop for remote NPCs:

```cpp
    // Also interpolate remote NPCs (from host sync)
    std::map<uint32_t, RemoteNPC>::iterator npc_it;
    for (npc_it = s_remote_npcs.begin(); npc_it != s_remote_npcs.end(); ++npc_it) {
        RemoteNPC& rnpc = npc_it->second;

        if (rnpc.interp_t < 1.0) {
            rnpc.interp_t += static_cast<double>(dt) * NPC_SYNC_RATE_HZ;
            if (rnpc.interp_t > 1.0) rnpc.interp_t = 1.0;
        }

        float t = static_cast<float>(rnpc.interp_t);
        float ix = lerp(rnpc.prev.x, rnpc.next.x, t);
        float iy = lerp(rnpc.prev.y, rnpc.next.y, t);
        float iz = lerp(rnpc.prev.z, rnpc.next.z, t);
        float iyaw = lerp_angle(rnpc.prev.yaw, rnpc.next.yaw, t);

        if (rnpc.npc) {
            Ogre::Vector3 pos(ix, iy, iz);
            Ogre::Quaternion rot(Ogre::Radian(iyaw), Ogre::Vector3::UNIT_Y);
            rnpc.npc->teleport(pos, rot);
        }
    }
```

- [ ] **Step 3: Update npc_manager_shutdown to clean up remote NPCs**

Add to `npc_manager_shutdown`, after the existing cleanup loop:

```cpp
    // Clean up remote NPCs
    for (it = s_remote_npcs.begin(); it != s_remote_npcs.end(); ++it) {
        if (it->second.npc && world) {
            world->destroy(it->second.npc, false, "KenshiMP shutdown");
        }
    }
    s_remote_npcs.clear();
```

Note: reuse the existing `world` variable and change the iterator type to match `RemoteNPC`:

```cpp
    std::map<uint32_t, RemoteNPC>::iterator rnpc_it;
    for (rnpc_it = s_remote_npcs.begin(); rnpc_it != s_remote_npcs.end(); ++rnpc_it) {
        if (rnpc_it->second.npc && world) {
            world->destroy(rnpc_it->second.npc, false, "KenshiMP shutdown");
        }
    }
    s_remote_npcs.clear();
```

- [ ] **Step 4: Commit**

```bash
git add core/src/npc_manager.cpp
git commit -m "feat(core): handle remote NPC spawn/state/despawn on joiner side"
```

---

### Task 5: Wire host_sync and NPC packets into player_sync.cpp

**Files:**
- Modify: `core/src/player_sync.cpp`

Connect the new host_sync and NPC packet handlers into the main tick loop and packet dispatch.

- [ ] **Step 1: Add extern declarations**

Add these extern declarations near the top of `player_sync.cpp`, after the existing externs:

```cpp
extern void host_sync_init();
extern void host_sync_shutdown();
extern void host_sync_tick(float dt);
extern void host_sync_set_host(bool is_host);
extern bool host_sync_is_host();

extern void npc_manager_on_remote_spawn(const NPCSpawnRemote& pkt);
extern void npc_manager_on_remote_state(const NPCStateEntry& entry);
extern void npc_manager_on_remote_despawn(uint32_t npc_id);
```

- [ ] **Step 2: Add NPC packet dispatch to on_packet_received**

Add these cases to the switch in `on_packet_received`, before the `default:` case:

```cpp
    case PacketType::NPC_SPAWN_REMOTE: {
        if (!host_sync_is_host()) {
            NPCSpawnRemote pkt;
            if (unpack(data, length, pkt)) {
                npc_manager_on_remote_spawn(pkt);
            }
        }
        break;
    }

    case PacketType::NPC_BATCH_STATE: {
        if (!host_sync_is_host()) {
            // Variable-length packet — manually parse
            NPCBatchHeader batch_hdr;
            if (unpack(data, length, batch_hdr)) {
                size_t offset = sizeof(NPCBatchHeader);
                for (uint16_t i = 0; i < batch_hdr.count; ++i) {
                    if (offset + sizeof(NPCStateEntry) > length) break;
                    NPCStateEntry entry;
                    std::memcpy(&entry, data + offset, sizeof(NPCStateEntry));
                    npc_manager_on_remote_state(entry);
                    offset += sizeof(NPCStateEntry);
                }
            }
        }
        break;
    }

    case PacketType::NPC_DESPAWN_REMOTE: {
        if (!host_sync_is_host()) {
            NPCDespawnRemote pkt;
            if (unpack(data, length, pkt)) {
                npc_manager_on_remote_despawn(pkt.npc_id);
            }
        }
        break;
    }
```

- [ ] **Step 3: Call host_sync_tick from player_sync_tick**

In `player_sync_tick`, add this call after `npc_manager_update(dt)`:

```cpp
    // Host: scan and send NPC state to server
    host_sync_tick(dt);
```

- [ ] **Step 4: Set is_host flag when connecting**

In the F9 auto-connect block and the auto-reconnect block, set `is_host` on the ConnectRequest. For now, read from a simple flag. Add a static at the top of player_sync_tick:

Actually, simpler: add is_host to the ConnectRequest in the F9 handler in `ui.cpp`. But first, set it during init. For now, the host is determined by a config file or hardcoded. Let's use a simple approach: the first player to connect is always the host.

In `player_sync_init`, add:
```cpp
    host_sync_init();
```

In the F9 connect code in `ui.cpp`, set `req.is_host = 1;` for now (all players claim host, server picks the first one). Update the F9 handler:

```cpp
    req.is_host = 1;  // first player becomes host
```

Also update the auto-reconnect in `player_sync.cpp`:

```cpp
    req.is_host = 1;
```

- [ ] **Step 5: Store host status from server response**

The server's CONNECT_ACCEPT doesn't currently tell the client if they're the host. For now, after connecting, always set host to true (since we only have one player). We'll refine this when multi-player testing begins.

After the `client_set_local_id(pkt.player_id)` line in CONNECT_ACCEPT handling:
```cpp
            host_sync_set_host(true);  // TODO: server should confirm host status
```

- [ ] **Step 6: Commit**

```bash
git add core/src/player_sync.cpp
git commit -m "feat(core): wire host_sync and NPC packet dispatch into main loop"
```

---

### Task 6: Update ui.cpp is_host flag in connect request

**Files:**
- Modify: `core/src/ui.cpp`

- [ ] **Step 1: Set is_host in F9 connect handler**

In `ui_check_hotkey`, in the F9 connect block, after the `std::strncpy(req.model, ...)` lines, add:

```cpp
                req.is_host = 1;
```

- [ ] **Step 2: Commit**

```bash
git add core/src/ui.cpp
git commit -m "feat(core): set is_host flag in connect request"
```

---

### Task 7: Build and test

**Files:**
- No code changes

- [ ] **Step 1: Rebuild core DLL with v100**

```bash
CMAKE="/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
DEPS="C:/Users/tlind/Documents/code/kenshi-mp/deps"

cd C:/Users/tlind/Documents/code/kenshi-mp
rm -rf build
"$CMAKE" -B build -T v100 -A x64 \
  -DENET_DIR="$DEPS/enet2" \
  -DKENSHILIB_DIR="$DEPS/KenshiLib" \
  -DKENSHILIB_EXAMPLES_DEPS="$DEPS/KenshiLib_Examples_deps" \
  -DBOOST_ROOT="$DEPS/KenshiLib_Examples_deps/boost_1_60_0" \
  -DKENSHIMP_BUILD_CORE=ON \
  -DKENSHIMP_BUILD_SERVER=OFF \
  -DKENSHIMP_BUILD_INJECTOR=OFF

cmake --build build --config Release --target KenshiMP
```

- [ ] **Step 2: Rebuild server**

```bash
cmake --build build_server --config Release
```

- [ ] **Step 3: Deploy and test**

```bash
cp build/core/Release/KenshiMP.dll "/c/Program Files (x86)/Steam/steamapps/common/Kenshi/mods/KenshiMP/"
```

Test sequence:
1. Start server
2. Launch Kenshi (host), press F9
3. Check server log: should show "Player 1 is the HOST"
4. Check kenshi.log: should show "[KenshiMP] This client is the HOST"
5. Walk near NPCs — server should receive NPC_SPAWN_REMOTE and NPC_BATCH_STATE packets
6. (Future: launch second Kenshi client to see NPCs spawn on joiner side)
