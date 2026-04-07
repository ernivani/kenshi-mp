---
name: KenshiMP Project Overview
description: Multiplayer mod for Kenshi — C++ Ogre3D plugin DLL, ENet UDP server, MyGUI overlay, injector launcher
type: project
---

Multiplayer mod for Kenshi (single-player RPG), following the Skyrim Together / JC2-MP pattern.

**Architecture:** Game loads KenshiMP.dll as Ogre3D plugin → reads player state via KenshiLib reverse-engineered API → syncs over ENet UDP at 20Hz → dedicated server relays with zone filtering.

**Components:** common (protocol), core (game plugin DLL), server (standalone relay), injector (launcher).

**Build:** Requires VS2010 x64 toolset, KenshiLib, ENet, Boost 1.60, Ogre. CMake-based.

**Why:** Adds multiplayer to a single-player game. Current scope: position/movement sync, chat, NPC spawning for remote players.

**How to apply:** Future work areas include combat sync, quest sync, inventory/trade, animation sync, auth/anti-cheat, world persistence.
