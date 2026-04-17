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

### 2026-04-17 — RE_Kenshi loose package installer rejects Steam 1.0.68
**Symptom:** `RE_Kenshi_v0.3.1_loose.zip` → `RE_Kenshi_installer.exe` shows "Hash 8a03c256f0da1555d9cceb939b41530a does not match. This mod is only compatible with Kenshi (Steam) and (GOG)" and the Next button stays disabled, even though that hash IS Steam 1.0.68 per the loose package's own `config.json`.

**Cause:** The loose installer.exe has a stale embedded hash list (or a bug). The loose `config.json` is read at runtime by the plugin, NOT by the installer.

**Workaround:** Use the main zip (`RE_Kenshi_v0.3.1.zip`, ~11 MB) which is a single all-in-one `RE_Kenshi_v0.3.1.exe` installer that embeds the up-to-date hash list and the courgette patch. It accepts Steam 1.0.68 and creates the downgraded `kenshi_x64_1_0_65.exe` automatically.

**How to apply:** When setting up RE_Kenshi on a fresh machine, download `RE_Kenshi_vX.Y.Z.zip` (NOT the `_loose.zip`). Run the installer, point at the Kenshi install dir, click Install. Don't try to manually copy the loose files — the installer also wires shortcuts, registry keys, and the auto-launch hook.

Manual loose-files install ALSO fails: even after copying `install/*` to Kenshi root and adding `Plugin=RE_Kenshi` to `Plugins_x64.cfg`, RE_Kenshi log shows "Version incompatible, restarting... UNKNOWN Unknown" + "KenshiLib could not detect Kenshi version" because the loose package only ships RVAs for 1.0.65 and the auto-restart-into-downgraded-exe trick the installer sets up isn't present.
