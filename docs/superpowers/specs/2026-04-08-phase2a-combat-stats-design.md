# Phase 2A: Combat Stats Sync — Design Spec

## Goal

Joiner's avatar on the host has correct combat stats so Kenshi's combat system produces accurate results. Host initiates combat on behalf of the joiner via `attackTarget()`.

## How It Works

### Stats sync (on connect):
1. Joiner reads own stats from `character->getStats()`
2. Sends PLAYER_COMBAT_STATS packet to host
3. Host applies stats to avatar NPC's CharStats by directly writing the float fields
4. Avatar now has correct strength, toughness, melee attack, etc.

### Combat initiation:
1. Joiner sends COMBAT_TARGET packet with target_npc_id when they want to fight
2. Host finds the real NPC and calls `avatar->attackTarget(npc)`
3. Kenshi's combat AI takes over — avatar walks to NPC, draws weapon, fights
4. All combat animations and damage run on host naturally
5. Results visible to joiner via NPC position/health batch sync

### Stat delta-sync:
Every 30 seconds, joiner re-reads stats and sends PLAYER_COMBAT_STATS again. Host updates avatar stats. Handles leveling up during gameplay.

## New Packets

### PLAYER_COMBAT_STATS (0x55) — reliable
Joiner → host. Sent on connect and every 30 seconds.
```
PacketHeader header;
uint32_t player_id;
float strength;
float dexterity;
float toughness;
float melee_attack;
float melee_defence;
float athletics;
float dodge;
float perception;
```

### COMBAT_TARGET (0x56) — reliable
Joiner → host. "I want to attack this NPC."
```
PacketHeader header;
uint32_t target_npc_id;
```

## Implementation

### Joiner side (player_sync.cpp):
- After connecting, read stats and send PLAYER_COMBAT_STATS
- Every 30 seconds, re-read and send
- F12 key (or proximity trigger): send COMBAT_TARGET for nearest synced NPC

### Host side (npc_manager.cpp or host_sync.cpp):
- On PLAYER_COMBAT_STATS: find avatar NPC, get its CharStats, write values
- On COMBAT_TARGET: find real NPC by npc_id, call `avatar->attackTarget(npc)`

### Server:
- Route PLAYER_COMBAT_STATS and COMBAT_TARGET from joiner to host (same as COMBAT_ATTACK handler)

## KenshiLib API

### Reading stats (joiner):
```cpp
CharStats* stats = character->getStats();
stats->_strength      // float at offset 0x80
stats->_dexterity     // float at offset 0x88
stats->_toughness     // float at offset 0x90
stats->__meleeAttack  // float at offset 0x120
stats->_meleeDefence  // float at offset 0x124
stats->_athletics     // float at offset 0x94
```

### Writing stats (host):
Same fields — directly writable.

### Combat initiation:
```cpp
avatar->attackTarget(targetNPC);  // Character::attackTarget(Character* who)
```

## What This Does NOT Include
- Weapon/armour equipping (avatar fights barehanded with martial arts)
- Appearance matching (avatar is still random)
- Ranged combat
- Health/knockout sync to joiner (future — needs safe getMedical access)
