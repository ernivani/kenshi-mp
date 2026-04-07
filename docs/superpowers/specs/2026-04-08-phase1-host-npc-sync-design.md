# Phase 1: Host Model + NPC Sync — Design Spec

## Goal

Joining players see the host's NPCs moving in real-time. The host's Kenshi client is the source of truth for all NPC state. Joiners mirror NPCs visually — no interaction (combat, inventory) in this phase.

## Architecture

```
Host Client                    Server                    Joiner Client
  |                              |                           |
  | reads NPCs from KenshiLib   |                           |
  | filters by proximity         |                           |
  |                              |                           |
  | NPC_SPAWN_REMOTE (new NPC)  |                           |
  |----------------------------->| relay to joiners          |
  |                              |-------------------------->| spawn NPC with race/gear
  |                              |                           |
  | NPC_BATCH_STATE (positions) |                           |
  |----------------------------->| relay to joiners          |
  |                              |-------------------------->| interpolate + teleport
  |                              |                           |
  | NPC_DESPAWN_REMOTE          |                           |
  |----------------------------->| relay to joiners          |
  |                              |-------------------------->| destroy NPC
```

- **Host client** reads NPC state from KenshiLib, sends to server
- **Server** relays NPC packets from host to all joiners (no NPC logic on server)
- **Joiner clients** spawn/move/despawn NPCs to mirror the host's world
- Joiners do NOT simulate NPCs locally — AI is disabled on synced NPCs

## New Packet Types

### NPC_SPAWN_REMOTE (0x41) — reliable

Sent by host when an NPC enters the sync radius of any player.

```
PacketHeader header;          // 2 bytes
uint32_t     npc_id;          // 4 bytes — host-assigned sequential ID
char         name[32];        // 32 bytes — NPC display name
char         race[32];        // 32 bytes — race name (greenlander, shek, etc.)
char         weapon[64];      // 64 bytes — equipped weapon model name
char         armour[64];      // 64 bytes — chest armour model name
float        x, y, z;        // 12 bytes — position
float        yaw;             // 4 bytes — rotation
```

Total: 214 bytes per spawn.

### NPC_BATCH_STATE (0x40) — unreliable

Sent by host at 10Hz. Contains position updates for all synced NPCs.

```
PacketHeader header;          // 2 bytes
uint16_t     count;           // 2 bytes — number of NPCs in batch
// Followed by `count` entries of:
struct NPCStateEntry {
    uint32_t npc_id;          // 4 bytes
    float    x, y, z;        // 12 bytes
    float    yaw;             // 4 bytes
    float    speed;           // 4 bytes
    uint32_t animation_id;   // 4 bytes
};                            // 28 bytes per NPC
```

Max batch size: limited by ENet MTU (~1400 bytes). With header overhead, ~49 NPCs per packet. If more NPCs are synced, split into multiple packets.

### NPC_DESPAWN_REMOTE (0x42) — reliable

Sent by host when an NPC leaves the sync radius or dies.

```
PacketHeader header;          // 2 bytes
uint32_t     npc_id;          // 4 bytes
```

## NPC ID System

The host maintains a lookup table mapping KenshiLib `hand` objects to compact sequential `uint32_t` IDs:

- `std::map<hand, uint32_t> s_hand_to_id` — hand → compact ID
- `std::map<uint32_t, hand> s_id_to_hand` — compact ID → hand
- `uint32_t s_next_npc_id = 1` — incrementing counter
- When an NPC enters sync range for the first time, assign the next ID
- IDs are never reused within a session (counter only goes up)

The joiner maintains a reverse map:
- `std::map<uint32_t, Character*> s_remote_npcs` — compact ID → spawned NPC pointer

## Host Sync Logic

New file: `core/src/host_sync.cpp`

### Per-tick logic (called from player_sync_tick):

1. Skip if not the host
2. Get all active characters via `ou->getCharacterUpdateList()`
3. For each NPC (skip player characters):
   a. Check if within 500 units of ANY connected player
   b. If in range and NOT in known set → assign npc_id, send NPC_SPAWN_REMOTE, add to known set
   c. If in range and in known set → add to batch state
   d. If NOT in range but in known set → send NPC_DESPAWN_REMOTE, remove from known set
4. Send NPC_BATCH_STATE with all in-range NPCs (at 10Hz, not every frame)

### Data structures:

```
struct SyncedNPC {
    uint32_t npc_id;
    hand     game_handle;     // KenshiLib handle
    float    last_x, last_y, last_z;  // for delta check
};
std::map<uint32_t, SyncedNPC> s_synced_npcs;  // currently synced NPCs
```

### Reading NPC appearance:

For NPC_SPAWN_REMOTE, read from KenshiLib:
- `name`: from Character's display name or game data
- `race`: from Character's race GameData name
- `weapon`: from equipped weapon's model/mesh name (if available)
- `armour`: from equipped chest armour's model/mesh name (if available)

If appearance data can't be read, send empty strings — joiner spawns a default character.

## Joiner NPC Management

Extend `core/src/npc_manager.cpp`:

### On NPC_SPAWN_REMOTE:
1. Look up race GameData by name (e.g. "greenlander" → find in `ou->gamedata`)
2. Call `factory->createRandomCharacter()` with the race template
3. Disable AI (`npc->ai = NULL`)
4. Store in `s_remote_npcs[npc_id] = npc`
5. Attempt to equip weapon/armour if names are provided (best-effort)

### On NPC_BATCH_STATE:
1. For each entry in the batch:
   a. Look up `s_remote_npcs[npc_id]`
   b. Update interpolation snapshots (same system as player NPCs)
   c. Teleport to interpolated position each frame

### On NPC_DESPAWN_REMOTE:
1. Look up `s_remote_npcs[npc_id]`
2. Call `world->destroy()` to remove
3. Erase from map

## Server Changes

### New packet routing:

The server needs to:
1. Track which player is the host (`is_host` flag in ConnectRequest)
2. When receiving NPC_SPAWN_REMOTE, NPC_BATCH_STATE, or NPC_DESPAWN_REMOTE from the host → relay to all joiners (not back to host)
3. Reject NPC packets from non-host players

### ConnectRequest change:

Add `uint8_t is_host` field to ConnectRequest packet. Server stores this in the session.

## Protocol Constants

```
static const float    NPC_SYNC_RADIUS     = 500.0f;   // units
static const float    NPC_SYNC_RATE_HZ    = 10.0f;    // batch updates per second
static const uint16_t MAX_NPC_BATCH_SIZE  = 49;       // per packet (MTU limit)
```

## Sync Radius

- 500 Kenshi units (roughly one town's worth of area)
- Host checks distance from each NPC to each connected player's last known position
- NPC is synced if within range of ANY player
- Host's own position counts as a player position

## What This Phase Does NOT Include

- No combat sync (damage, knockouts, deaths)
- No inventory/item sync
- No building sync
- No quest/event sync
- No NPC AI on joiner side (NPCs are puppets)
- No full appearance cloning (just race + weapon + armour)
- No NPC interaction (joiner can't talk to, recruit, or trade with synced NPCs)

## File Changes Summary

| File | Change |
|------|--------|
| `common/include/packets.h` | Add NPC_SPAWN_REMOTE, NPC_BATCH_STATE, NPC_DESPAWN_REMOTE packet types and structs |
| `common/include/protocol.h` | Add NPC_SYNC_RADIUS, NPC_SYNC_RATE_HZ, MAX_NPC_BATCH_SIZE constants |
| `core/src/host_sync.cpp` | New file: host-side NPC scanning, ID assignment, proximity filtering, batch sending |
| `core/src/npc_manager.cpp` | Extend: handle NPC_SPAWN_REMOTE, NPC_BATCH_STATE, NPC_DESPAWN_REMOTE |
| `core/src/player_sync.cpp` | Call host_sync_tick(), dispatch new packet types |
| `core/CMakeLists.txt` | Add host_sync.cpp to build |
| `server/src/session.cpp` | Track host player, route NPC packets, add is_host to connect flow |
| `common/include/packets.h` | Add is_host field to ConnectRequest |

## Testing

1. Start server
2. Launch host Kenshi, press F9 (with is_host config)
3. Launch joiner Kenshi, press F9 (without is_host)
4. Host walks near NPCs → joiner should see them spawn
5. Host walks away → joiner should see them despawn
6. NPCs moving on host → joiner sees them move in real-time
7. Verify no crash on spawn/despawn at zone boundaries
