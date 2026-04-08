---
name: Session Progress April 8 2026 Afternoon
description: Character appearance matching, faction fix (Drifters), yaw sync, F9/F11 removed, debug cleanup
type: project
---

## Changes Made

### Character Appearance Matching
- Host sends character's `GameData::stringID` in spawn packets
- For NPCs: sent in the `weapon` field of NPCSpawnRemote (repurposed)
- For players: sent in the `model` field of ConnectRequest
- Joiner uses `factory->create(gameData, pos, ...)` to spawn exact character template
- Falls back to `createRandomCharacter` if template not found
- Player UI reads `local_ch->data->stringID` when connecting

### Faction Fix — Drifters
- `getFactionByName("Drifters")` works — NPCs spawn as neutral Drifters
- Custom factions (getOrCreateFaction) crash createRandomCharacter
- getEmptyFaction also crashes createRandomCharacter
- Player faction works but NPCs join your team
- **Solution**: try Drifters → Traders Guild → Wanderer → Tech Hunters → fallback to player faction

### Player/NPC Yaw (Rotation)
- Compute yaw from `ch->getMovementDirection()` using `atan2(dir.x, dir.z)`
- Keep last known yaw when character is stationary
- Applied to both player state and NPC batch state

### NPC Movement Animations
- Use `setDestination(target, HIGH_PRIORITY, false)` instead of `teleport()` for nearby NPCs
- NPCs walk naturally with animations when distance < 100 units
- Teleport for large distances (>100 units)
- Also applied to player avatar NPCs (distance threshold: 50 units)
- `CharMovement::setDestination()` gives pathfinding + walking animation

### UI Cleanup
- Removed F9 (host connect) and F11 (joiner connect) hotkeys
- Only F8 (Host/Join/Disconnect dialog) and F10 (admin panel) remain
- Cleaned verbose per-packet debug logging

### Local NPC Hiding
- Re-enabled continuous hiding with std::set for synced NPC protection
- Uses `std::set<Character*> our_npcs` for O(1) lookup
- Hides any NPC with y > -90000 that isn't in our synced set

## Key Findings

### factory->create() vs createRandomCharacter()
- `create(GameData*, pos, false, faction, rot, NULL, NULL, NULL, false, NULL, 0.0f)` spawns specific character
- Needs GameData from `ou->gamedata.gamedataSID[stringID]`
- Works for both NPCs and player avatars
- createRandomCharacter is fallback only

### KenshiLib Movement API
- `CharMovement* movement = ch->getMovement()` — get movement controller
- `movement->setDestination(Vector3, HIGH_PRIORITY, false)` — pathfind + walk
- `movement->isIdle()` / `movement->isRunning()` — state checks
- `movement->faceDirection(Vector3)` — face a direction
- `ch->getMovementDirection()` — current movement vector
- `ch->getMovementSpeed()` — current speed

### UpdatePriority Enum
- LOW_PRIORITY, MED_PRIORITY, HIGH_PRIORITY (no URGENT)
