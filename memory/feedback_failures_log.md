---
name: KenshiMP failures log
description: Things that have been tried and failed during implementation — runtime crashes, KenshiLib API quirks, build issues, what NOT to do
type: feedback
---

Log of every approach tried that did NOT work, so we don't retry the same dead-ends. Add an entry every time something crashes, doesn't behave, or has to be reverted. Keep it terse — date + symptom + cause + workaround if any.

## How to apply
Before trying a new KenshiLib API or pattern, grep this file. If the symptom matches, use the documented workaround instead of re-discovering the failure.

## Why
KenshiLib is undocumented. Every crash costs 10+ minutes of game restart + hook reload. Logging failures pays for itself within 2-3 sessions.

---

## Entries

### 2026-04-17 — `ou->destroy()` on bulk world buildings crashes the joiner
**Symptom:** Joiner crashes inside `building_manager_hide_local_buildings()` immediately after `Hidden N local NPCs` log line. Crash dump: `crashDump1.0.65_x64.dmp` ~950KB.

**Cause:** Calling `ou->destroy(obj, false, "KenshiMP")` on every BUILDING/ITEM returned by `getObjectsWithinSphere` (~500+ objects) breaks the game's internal world state — many buildings belong to towns / squad ownerships and destroying them mid-iteration corrupts pointers.

**DO NOT:** loop over `getObjectsWithinSphere(BUILDING, ...)` results and call `ou->destroy()` to wipe the world. Same for `dynamicDestroyBuilding`.

**USE INSTEAD:** `obj->setVisible(false)` (RootObject pure virtual at vtable offset 0x100) — does not crash. Limitation: setVisible only hides the main mesh; LOD imposters / terrain-baked decoration stay visible. For full invisibility we'd need to also disable Ogre scene-node rendering or teleport the buildings (untested).

**SUPERSEDED** by entries below — `destroy` and `dynamicDestroyBuilding` DO work, but only one at a time (batch=1). See 2026-04-18 entries.

---

### 2026-04-17 — RE_Kenshi loose package installer rejects Steam 1.0.68
**Symptom:** `RE_Kenshi_v0.3.1_loose.zip` → `RE_Kenshi_installer.exe` shows "Hash 8a03c256f0da1555d9cceb939b41530a does not match. This mod is only compatible with Kenshi (Steam) and (GOG)" and the Next button stays disabled, even though that hash IS Steam 1.0.68 per the loose package's own `config.json`.

**Cause:** The loose installer.exe has a stale embedded hash list (or a bug). The loose `config.json` is read at runtime by the plugin, NOT by the installer.

**Workaround:** Use the main zip (`RE_Kenshi_v0.3.1.zip`, ~11 MB) which is a single all-in-one `RE_Kenshi_v0.3.1.exe` installer that embeds the up-to-date hash list and the courgette patch. It accepts Steam 1.0.68 and creates the downgraded `kenshi_x64_1_0_65.exe` automatically.

**How to apply:** When setting up RE_Kenshi on a fresh machine, download `RE_Kenshi_vX.Y.Z.zip` (NOT the `_loose.zip`). Run the installer, point at the Kenshi install dir, click Install. Don't try to manually copy the loose files — the installer also wires shortcuts, registry keys, and the auto-launch hook.

Manual loose-files install ALSO fails: even after copying `install/*` to Kenshi root and adding `Plugin=RE_Kenshi` to `Plugins_x64.cfg`, RE_Kenshi log shows "Version incompatible, restarting... UNKNOWN Unknown" + "KenshiLib could not detect Kenshi version" because the loose package only ships RVAs for 1.0.65 and the auto-restart-into-downgraded-exe trick the installer sets up isn't present.

---

### 2026-04-18 — `ou->destroy()` leaves physics/collision bodies behind
**Symptom:** Buildings disappear visually but joiner character is blocked by invisible collision walls where buildings used to be.

**Cause:** `ou->destroy(obj, false, "KenshiMP")` removes the visual mesh but does NOT clean up the PhysX/Havok collision bodies. Calling `obj->destroyPhysical()` before `ou->destroy()` also doesn't help.

**Fix:** Use `ou->dynamicDestroyBuilding(hand(obj))` for buildings — this is the game's native building demolition path and properly tears down collision. Keep `ou->destroy()` for items only (no significant collision).

---

### 2026-04-18 — Batch destroying buildings crashes (even with dynamicDestroyBuilding)
**Symptom:** Destroying >1 building per scan tick crashes. Tested with both `ou->destroy()` (batch=5) and `dynamicDestroyBuilding()` (batch=5). Both crash mid-batch.

**Cause:** Cascade invalidation — destroying one building can cascade-free related buildings that are still in the scan results array. The second/third/etc destroy in the same batch then hits freed memory.

**DO NOT:** Set `BATCH_PER_TICK` above 1 in `building_manager.cpp`.

**USE INSTEAD:** `BATCH_PER_TICK=1` with `WIPE_INTERVAL=0.1f` (100ms). This gives ~10 destroys/sec throughput. Each destroy gets its own fresh scan, so cascade invalidation can't corrupt the results. To increase throughput, reduce `WIPE_INTERVAL` — never increase batch size.
