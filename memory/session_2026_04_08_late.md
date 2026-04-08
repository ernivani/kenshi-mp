---
name: Session Progress April 8 2026 Late
description: Appearance matching experiments, AI control learnings, stable state identified
type: project
---

## What Was Tried and Failed

### factory->create() for appearance matching
- Sent character's `GameData::stringID` in spawn packets
- Used `factory->create(gameData, pos, ...)` instead of `createRandomCharacter`
- Result: spawned NPCs had full AI enabled and wandered around
- The created characters weren't controllable via `setDestination` properly
- **Reverted** — needs more research into how to spawn puppet characters

### Custom faction (getOrCreateFaction)
- `ou->factionMgr->getOrCreateFaction("KenshiMP_NPCs", "KenshiMP NPCs")` — creates faction
- `createRandomCharacter` with this faction → crashes
- **Reason**: empty custom factions have no character templates
- **Reverted** — custom factions don't work with createRandomCharacter

### halt() every frame
- Called `movement->halt()` every frame before `setDestination()`
- Result: NPCs never walked — halt cancelled the setDestination from previous frame
- Characters just twitched/teleported instead of walking
- **Lesson**: halt() cancels ALL movement including our setDestination
- Only use halt() before teleport (far distances), never before setDestination

## Stable Working State (commit ed01d17 / e473096)

### What works:
- Players see each other walking with yaw sync
- Joiner sees host's 18 NPCs walking with animations
- NPCs use Drifters faction (neutral, not player team)
- setDestination(HIGH_PRIORITY) for movement — game handles walking animation
- Teleport for large distances (>50/100 units)
- Local NPCs hidden on joiner (initial hide + continuous hide with set-based protection)
- Clean logs, no spam

### Key architecture decisions (confirmed working):
- `createRandomCharacter(Drifters_faction, pos, NULL, NULL, NULL, 0.0f)` for NPC spawning
- `setDestination(target, HIGH_PRIORITY, false)` for movement — DO NOT call halt() before
- `teleport(pos, rot)` only for large distances
- `ch->getMovementDirection()` + `atan2(dir.x, dir.z)` for yaw
- `getEmptyFaction()` → player faction fallback for player avatar NPCs

## Known Issues (Not Fixed)
- NPCs are random appearance (not matching host's actual NPCs)
- Player avatars are random appearance
- NPCs join player team when using player faction (player avatar spawn)
- No combat sync
- Appearance matching needs different approach than factory->create()
  - Maybe: serialize appearance data via getAppearanceData() + giveBirth()
  - Or: find a way to spawn specific race with createRandomCharacter template param

## Key Lesson: Don't Fight Kenshi's AI

The game's AI system is deeply integrated. Approaches that break it:
- `npc->ai = NULL` → crash next frame
- `halt()` every frame → prevents walking animation
- `factory->create()` → full AI enabled, not controllable
- Custom empty factions → crash createRandomCharacter

Approaches that work WITH the AI:
- `setDestination(HIGH_PRIORITY)` → overrides AI destination naturally
- `Drifters` faction → neutral, has character templates
- `createRandomCharacter` → minimal AI that doesn't fight setDestination
