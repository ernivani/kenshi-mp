# Phase 2: Combat Sync — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Joiner can attack host's NPCs (host-authoritative damage). Host's NPCs can attack joiner. NPC health/knockout/death synced in batch state.

**Architecture:** Joiner proximity-detects attacks on synced NPCs, sends COMBAT_ATTACK to host. Host applies damage via `hitByMeleeAttack()`, syncs health state in extended NPC_BATCH_STATE. Host detects damage to joiner's avatar NPC, sends COMBAT_DAMAGE to joiner. Joiner applies damage to own character.

**Tech Stack:** C++11 (v100 toolset), KenshiLib (Damages, MedicalSystem, Character::hitByMeleeAttack), ENet

**v100 rules:** NULL not nullptr, explicit iterators, std::ostringstream for itos, no auto, no enum class.

---

### Task 1: Add combat packets and extend NPCStateEntry with health

**Files:**
- Modify: `common/include/packets.h`

- [ ] **Step 1: Add combat packet types and structs**

In `common/include/packets.h`, add after `NPC_DESPAWN_REMOTE`:

```cpp
    static const uint8_t COMBAT_ATTACK      = 0x50;
    static const uint8_t COMBAT_DAMAGE      = 0x51;
```

Add these structs before `#pragma pack(pop)`:

```cpp
struct CombatAttack {
    PacketHeader header;
    uint32_t     target_npc_id;
    float        cut_damage;
    float        blunt_damage;
    float        pierce_damage;

    CombatAttack() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::COMBAT_ATTACK;
    }
};

struct CombatDamage {
    PacketHeader header;
    uint32_t     player_id;
    float        cut_damage;
    float        blunt_damage;
    float        pierce_damage;

    CombatDamage() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::COMBAT_DAMAGE;
    }
};
```

- [ ] **Step 2: Extend NPCStateEntry with health/flags**

Replace the existing `NPCStateEntry` with:

```cpp
struct NPCStateEntry {
    uint32_t npc_id;
    float    x, y, z;
    float    yaw;
    float    speed;
    uint32_t animation_id;
    uint8_t  flags;            // bit 0: isDown, bit 1: isDead
    int16_t  health_percent;   // 0-100
};
```

- [ ] **Step 3: Commit**

```bash
git add common/include/packets.h
git commit -m "feat(proto): add COMBAT_ATTACK/DAMAGE packets, health in NPCStateEntry"
```

---

### Task 2: Host reads and sends NPC health state

**Files:**
- Modify: `core/src/host_sync.cpp`

- [ ] **Step 1: Include MedicalSystem header**

Add at the top of host_sync.cpp:
```cpp
#include <kenshi/MedicalSystem.h>
```

- [ ] **Step 2: Fill health/flags in NPC batch state**

In `host_sync_tick`, in the batch building loop where `NPCStateEntry entry` is populated, after `entry.animation_id = 0;` add:

```cpp
        // Health state
        entry.flags = 0;
        if (ch->isDown()) entry.flags |= 0x01;
        if (ch->isDead()) entry.flags |= 0x02;

        // Health percent from first body part (head/chest)
        entry.health_percent = 100;
        MedicalSystem* med = ch->getMedical();
        if (med && med->anatomy.count > 0) {
            MedicalSystem::HealthPartStatus* part = med->anatomy.stuff[0];
            if (part) {
                float maxHp = part->maxHealth();
                if (maxHp > 0.0f) {
                    float currentHp = part->_maxHealth; // current value
                    entry.health_percent = static_cast<int16_t>((currentHp / maxHp) * 100.0f);
                    if (entry.health_percent < 0) entry.health_percent = 0;
                    if (entry.health_percent > 100) entry.health_percent = 100;
                }
            }
        }
```

- [ ] **Step 3: Commit**

```bash
git add core/src/host_sync.cpp
git commit -m "feat(core): send NPC health/knockout/death state in batch"
```

---

### Task 3: Host receives COMBAT_ATTACK and applies damage

**Files:**
- Modify: `core/src/player_sync.cpp`
- Modify: `core/src/host_sync.cpp`
- Modify: `server/src/session.cpp`

- [ ] **Step 1: Add server relay for combat packets**

In `server/src/session.cpp`, in `session_on_packet` switch, add before `default:`:

```cpp
    case PacketType::COMBAT_ATTACK:
        handle_npc_packet(peer, data, length, true);
        break;
    case PacketType::COMBAT_DAMAGE:
        handle_npc_packet(peer, data, length, true);
        break;
```

Note: COMBAT_ATTACK goes from joiner → server → host (via handle_npc_packet which relays to host... wait, handle_npc_packet relays FROM host. We need a new handler that relays TO host.)

Actually, COMBAT_ATTACK goes joiner → server → host. The server needs to forward it to the host. Add a new handler:

```cpp
static void handle_combat_to_host(ENetPeer* peer, const uint8_t* data, size_t length) {
    auto it = s_peer_to_id.find(peer);
    if (it == s_peer_to_id.end()) return;

    // Forward to host
    if (s_host_id == 0) return;

    // Find host peer
    for (auto& pair : s_sessions) {
        if (pair.first == s_host_id) {
            relay_send_to(pair.second.peer, data, length, true);
            break;
        }
    }

    s_sessions[it->second].last_activity = std::chrono::steady_clock::now();
}
```

Then in the switch:
```cpp
    case PacketType::COMBAT_ATTACK:
        handle_combat_to_host(peer, data, length);
        break;
    case PacketType::COMBAT_DAMAGE:
        handle_npc_packet(peer, data, length, true);  // host → joiners
        break;
```

- [ ] **Step 2: Add apply damage function in host_sync.cpp**

Add to host_sync.cpp:

```cpp
#include <kenshi/Damages.h>
#include <kenshi/Enums.h>

void host_sync_on_combat_attack(const CombatAttack& pkt) {
    if (!s_is_host) return;
    if (!ou) return;

    // Find the real NPC by npc_id
    Character* target = NULL;
    const ogre_unordered_set<Character*>::type& chars = ou->getCharacterUpdateList();
    std::map<uint64_t, SyncedNPC>::iterator it;
    for (it = s_synced_npcs.begin(); it != s_synced_npcs.end(); ++it) {
        if (it->second.npc_id == pkt.target_npc_id) {
            target = (Character*)(uintptr_t)it->first;
            // Verify still in update list
            bool valid = false;
            ogre_unordered_set<Character*>::type::const_iterator cit;
            for (cit = chars.begin(); cit != chars.end(); ++cit) {
                if (*cit == target) { valid = true; break; }
            }
            if (!valid) target = NULL;
            break;
        }
    }

    if (!target) {
        KMP_LOG("[KenshiMP] Combat: target NPC " + itos(pkt.target_npc_id) + " not found");
        return;
    }

    // Apply damage
    Damages dmg(pkt.cut_damage, pkt.blunt_damage, pkt.pierce_damage, 0.0f, 0.0f);
    target->hitByMeleeAttack(CUT_DEFAULT, dmg, NULL, NULL, 0);

    KMP_LOG("[KenshiMP] Combat: applied damage to NPC " + itos(pkt.target_npc_id));
}
```

- [ ] **Step 3: Wire combat packet dispatch in player_sync.cpp**

Add extern:
```cpp
extern void host_sync_on_combat_attack(const CombatAttack& pkt);
```

Add to `on_packet_received` switch, before `default:`:
```cpp
    case PacketType::COMBAT_ATTACK: {
        if (host_sync_is_host()) {
            CombatAttack pkt;
            if (unpack(data, length, pkt)) {
                host_sync_on_combat_attack(pkt);
            }
        }
        break;
    }
```

- [ ] **Step 4: Commit**

```bash
git add core/src/host_sync.cpp core/src/player_sync.cpp server/src/session.cpp
git commit -m "feat(core): host receives COMBAT_ATTACK and applies damage to NPCs"
```

---

### Task 4: Joiner detects attacks and sends COMBAT_ATTACK

**Files:**
- Modify: `core/src/npc_manager.cpp`

- [ ] **Step 1: Add proximity-based attack detection**

In `npc_manager_update`, after the remote NPC section, add joiner-side attack detection. Each tick, check if joiner's player character is close to any synced NPC and has positive speed (moving toward them = attacking):

Add externs at top of npc_manager.cpp:
```cpp
extern void client_send_reliable(const uint8_t* data, size_t length);
extern bool client_is_connected();
extern bool host_sync_is_host();
extern Character* game_get_player_character();
```

Add a static timer and the detection logic at the end of `npc_manager_update`:

```cpp
    // Joiner: proximity-based attack detection
    if (!host_sync_is_host() && client_is_connected()) {
        static float s_attack_cooldown = 0.0f;
        s_attack_cooldown -= dt;

        if (s_attack_cooldown <= 0.0f) {
            Character* player = game_get_player_character();
            if (player && player->getMovementSpeed() > 5.0f) {
                Ogre::Vector3 player_pos = player->getPosition();

                std::map<uint32_t, RemoteNPC>::iterator npc_it;
                for (npc_it = s_remote_npcs.begin(); npc_it != s_remote_npcs.end(); ++npc_it) {
                    if (!npc_it->second.npc) continue;

                    Ogre::Vector3 npc_pos = npc_it->second.npc->getPosition();
                    float dx = player_pos.x - npc_pos.x;
                    float dz = player_pos.z - npc_pos.z;
                    float dist_sq = dx*dx + dz*dz;

                    if (dist_sq < 8.0f * 8.0f) {  // within 8 units = melee range
                        CombatAttack atk;
                        atk.target_npc_id = npc_it->second.npc_id;
                        atk.cut_damage = 20.0f;    // base damage values
                        atk.blunt_damage = 10.0f;
                        atk.pierce_damage = 0.0f;

                        std::vector<uint8_t> buf = pack(atk);
                        client_send_reliable(buf.data(), buf.size());

                        s_attack_cooldown = 1.0f;  // 1 attack per second
                        break;  // only attack one NPC per cooldown
                    }
                }
            }
        }
    }
```

- [ ] **Step 2: Commit**

```bash
git add core/src/npc_manager.cpp
git commit -m "feat(core): joiner proximity-detects attacks and sends COMBAT_ATTACK"
```

---

### Task 5: Host detects damage to joiner avatar and sends COMBAT_DAMAGE

**Files:**
- Modify: `core/src/host_sync.cpp`
- Modify: `core/src/npc_manager.cpp`
- Modify: `core/src/player_sync.cpp`

- [ ] **Step 1: Track joiner avatar health on host**

In host_sync.cpp, add tracking for joiner avatar health. Each tick, check if the avatar NPC's health changed:

Add a static to track previous health:
```cpp
static float s_avatar_last_health = -1.0f;
```

At the end of `host_sync_tick`, after the batch sending, add:

```cpp
    // Check if joiner's avatar NPC took damage
    // (npc_manager manages player avatars — we need to access them)
    // This is handled separately by checking avatar NPC health each tick
```

Actually, it's simpler to do this in npc_manager_update on the host side — the host has the avatar NPC pointer. Add to `npc_manager_update`, after the player interpolation loop:

```cpp
    // Host: detect damage to remote player avatars
    if (host_sync_is_host() && client_is_connected()) {
        for (it = s_remote_players.begin(); it != s_remote_players.end(); ++it) {
            RemotePlayer& rp = it->second;
            if (!rp.npc) continue;

            MedicalSystem* med = rp.npc->getMedical();
            if (!med || med->anatomy.count <= 0) continue;

            MedicalSystem::HealthPartStatus* part = med->anatomy.stuff[0];
            if (!part) continue;

            float current_hp = part->_maxHealth;

            // Check if health decreased since last check
            if (rp.last_health >= 0.0f && current_hp < rp.last_health) {
                float damage = rp.last_health - current_hp;

                CombatDamage dmg_pkt;
                dmg_pkt.player_id = rp.player_id;
                dmg_pkt.cut_damage = damage * 0.5f;
                dmg_pkt.blunt_damage = damage * 0.5f;
                dmg_pkt.pierce_damage = 0.0f;

                std::vector<uint8_t> buf = pack(dmg_pkt);
                client_send_reliable(buf.data(), buf.size());

                KMP_LOG("[KenshiMP] Combat: avatar took " + itos(static_cast<uint32_t>(damage)) + " damage");
            }

            rp.last_health = current_hp;
        }
    }
```

This requires adding `float last_health;` to the `RemotePlayer` struct (init to -1.0f) and including `<kenshi/MedicalSystem.h>`.

- [ ] **Step 2: Add last_health to RemotePlayer struct**

In npc_manager.cpp, add to `RemotePlayer`:
```cpp
    float   last_health;
```

In `npc_manager_on_spawn`, init it:
```cpp
    rp.last_health = -1.0f;
```

- [ ] **Step 3: Wire COMBAT_DAMAGE dispatch on joiner**

In player_sync.cpp, add to `on_packet_received` switch:

```cpp
    case PacketType::COMBAT_DAMAGE: {
        if (!host_sync_is_host()) {
            CombatDamage pkt;
            if (unpack(data, length, pkt)) {
                // Apply damage to local player character
                Character* player = game_get_player_character();
                if (player) {
                    Damages dmg(pkt.cut_damage, pkt.blunt_damage, pkt.pierce_damage, 0.0f, 0.0f);
                    player->hitByMeleeAttack(CUT_DEFAULT, dmg, NULL, NULL, 0);
                    KMP_LOG("[KenshiMP] Combat: took damage from host NPC");
                }
            }
        }
        break;
    }
```

Add includes for Damages and Enums at top of player_sync.cpp:
```cpp
#include <kenshi/Damages.h>
#include <kenshi/Enums.h>
```

- [ ] **Step 4: Commit**

```bash
git add core/src/npc_manager.cpp core/src/player_sync.cpp
git commit -m "feat(core): host detects avatar damage, joiner receives COMBAT_DAMAGE"
```

---

### Task 6: Build, deploy, and test

- [ ] **Step 1: Build core DLL**

```bash
cmake --build build --config Release --target KenshiMP
```

- [ ] **Step 2: Rebuild server**

```bash
cmake --build build_server --config Release
```

- [ ] **Step 3: Deploy**

```bash
cp build/core/Release/KenshiMP.dll "mods/KenshiMP/"
cp build_server/server/Release/kenshi-mp-server.exe "mods/KenshiMP/"
```

- [ ] **Step 4: Test**

1. Start two Kenshi instances (Host + Join)
2. Host walks near NPCs — joiner should see them with health data
3. Joiner walks up to a synced NPC and moves toward it — should trigger COMBAT_ATTACK
4. Host should see the NPC take damage
5. Host NPC attacks joiner avatar — joiner should take damage
6. Check logs for COMBAT messages
