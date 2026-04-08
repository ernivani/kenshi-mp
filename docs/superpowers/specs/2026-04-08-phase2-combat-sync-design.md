# Phase 2: Combat Sync — Design Spec

## Goal

Joiner can attack host's NPCs and see damage results. Host's NPCs can attack the joiner. All damage is calculated on the host (authoritative). This is step A toward full co-op combat (C).

## Architecture

```
Joiner attacks synced NPC:
  Joiner game detects hit → COMBAT_ATTACK packet → Server → Host
  Host applies hitByMeleeAttack() on real NPC → damage/knockout/death
  Host syncs NPC health state via extended NPC_BATCH_STATE → Server → Joiner
  Joiner sees NPC react (health bar, knockout, death)

Host NPC attacks joiner:
  Host detects damage to joiner's player avatar NPC → COMBAT_DAMAGE packet → Server → Joiner
  Joiner applies damage to own character via medical.addWound()
  Joiner sees their own health decrease
```

## New Packets

### COMBAT_ATTACK (0x50) — joiner → host, reliable
Joiner's character hit a synced NPC.
```
PacketHeader header;
uint32_t     target_npc_id;    // which synced NPC was hit
float        cut_damage;       // damage values (from joiner's weapon stats)
float        blunt_damage;
float        pierce_damage;
uint8_t      body_part;        // which body part (0=random)
```

### COMBAT_DAMAGE (0x51) — host → joiner, reliable
Joiner's player avatar took damage on the host.
```
PacketHeader header;
uint32_t     player_id;        // which player took damage
float        cut_damage;
float        blunt_damage;
float        pierce_damage;
uint8_t      body_part;
```

### Extended NPCStateEntry
Add to existing NPC batch state:
```
uint8_t      flags;            // bit 0: isDown, bit 1: isDead
int16_t      health_percent;   // 0-100, main body part health
```

## Joiner Side: Detecting Attacks

### Approach: Poll damage events
Each tick on the joiner, check if any synced NPC's health decreased since last tick. If yes, the joiner's character attacked it.

Problem: synced NPCs spawned with `createRandomCharacter` are separate from Kenshi's combat system. The joiner's character can't actually hit them because they're in the Drifters faction (neutral/friendly).

### Better approach: Use setDestination to make joiner attack
When the joiner right-clicks a synced NPC, instead of normal Kenshi combat:
1. Detect the right-click on a synced NPC
2. Send COMBAT_ATTACK to host with the joiner's weapon damage stats
3. Host applies damage to the real NPC

Problem: intercepting right-clicks on specific NPCs is complex.

### Simplest approach for v1: Proximity-based auto-attack
Don't try to intercept Kenshi's combat system. Instead:
1. Each tick, check if joiner's player character is in melee range of any synced NPC (< 5 units)
2. Check if the joiner is in "combat stance" (has weapon drawn, isRunning toward NPC)
3. Send COMBAT_ATTACK at a rate limit (e.g. 1 per second)
4. Host applies damage

This is imprecise but gets combat working without hooking the combat system.

### Recommended approach: Hook hitByMeleeAttack
Hook `Character::hitByMeleeAttack` on the joiner side. When a synced NPC is "hit" by the joiner's character locally, intercept the damage values and send them to the host instead of applying locally.

This requires:
- The joiner's synced NPCs to be attackable (faction must allow combat)
- Hooking a virtual function (same GetProcAddress trick as mainLoop)
- Intercepting the damage before it's applied

This is the cleanest approach but requires research into whether Drifters faction NPCs are attackable.

## Host Side: Applying Damage

When host receives COMBAT_ATTACK:
1. Look up the real NPC by npc_id
2. Create a `Damages` struct from the packet values
3. Call `character->hitByMeleeAttack()` or `medical.addWound()`
4. The host's game handles all consequences (knockout, death, ragdoll, AI reaction)

## Host Side: Detecting Damage to Joiner's Avatar

The joiner's avatar on the host is a spawned NPC. When host NPCs attack it:
1. Each tick, check the avatar NPC's health state
2. If health decreased, calculate the delta and send COMBAT_DAMAGE to joiner
3. Or: hook hitByMeleeAttack on the avatar NPC to detect incoming damage

## Joiner Side: Receiving Damage

When joiner receives COMBAT_DAMAGE:
1. Apply damage to the joiner's own player character via `medical.addWound()` or `medical.applyDamage()`
2. The joiner's game handles health bars, knockout, death naturally

## Syncing NPC Health State

Extend NPC_BATCH_STATE with health/status flags:
- `health_percent`: overall health as 0-100
- `flags`: isDown (knockout), isDead
- Read from `character->isDown()`, `character->isDead()`
- Read health from `medical.anatomy` (main body part)

Joiner can display health bars or knockout state on synced NPCs.

## Implementation Order

1. **Health sync in batch state** — add health/flags to NPCStateEntry, read on host, display on joiner
2. **Joiner attacks host NPC** — COMBAT_ATTACK packet, host applies damage
3. **NPC attacks joiner** — COMBAT_DAMAGE packet, detect on host, apply on joiner
4. **Combat animations** — sync attack/block animations (future)

## What This Phase Does NOT Include

- Combat animations on synced NPCs (they take damage but don't show attack animations)
- Block/dodge mechanics
- Ranged combat (crossbows)
- Squad combat (multiple player characters)
- Joiner-to-joiner PvP

## KenshiLib API Reference

```
// Apply melee damage
HitMaterialType Character::hitByMeleeAttack(CutDirection dir, Damages& damage, Character* who, CombatTechniqueData* attack, int comboID);

// Damage struct
Damages(float cut, float blunt, float pierce, float bleed, float armour);

// Health access
MedicalSystem* Character::getMedical();
lektor<MedicalSystem::HealthPartStatus*> medical.anatomy;
float HealthPartStatus::maxHealth();
void HealthPartStatus::applyDamage(const Damages& damage);
void MedicalSystem::addWound(bool lowBlow, CutDirection area, Damages& damage, ...);

// State checks
bool Character::isDown();
bool Character::isUnconcious();
bool Character::isDead();
void Character::declareDead();
void Character::healCompletely();
```
