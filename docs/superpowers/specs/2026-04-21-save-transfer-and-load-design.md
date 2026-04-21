# Spec A — Save Transfer & Load Path

**Status:** Draft
**Date:** 2026-04-21
**Part of:** Minecraft-style persistent multiplayer rework (scope: this spec covers bulk save transfer + load only; faction/character lifecycle, auto-save, and runtime-authority are separate specs).

## Goal

When a joiner connects to a host, the joiner's Kenshi client downloads the host's current save, loads it via Kenshi's native import path, and ends up running in the exact same world as the host. After this spec ships, both clients have byte-identical world state at load time. Runtime sync (which comes from existing packet code + later specs) takes over from there.

This is the foundation of the persistent-world rework: every other piece (per-player factions, auto-save, runtime authority) assumes the two sides start from the same save.

## Non-goals (explicitly out of scope)

- Per-player faction creation and character lifecycle → Spec B.
- Character creator UI on first join → Spec B.
- Auto-save scheduling and `WorldSaved` broadcast → Spec D.
- Runtime authority (who wins on conflict) → Spec E.
- Delta/incremental snapshots → future optimisation, MVP does full transfer every join.
- Mod-list validation → future, out of scope.
- Authentication on the snapshot download → future, MVP assumes trusted LAN/friends.

## User-visible flow

1. Host clicks **Multiplayer → Host** in the main menu, picks a local save to host, clicks confirm.
2. Host's Kenshi loads that save normally (via `TitleScreen::loadGame`). Plugin spawns the server process. Server saves the world once (via a save-trigger hook) and keeps a zipped copy of the save folder in RAM as "snapshot rev 1".
3. Joiner clicks **Multiplayer → Join**, enters IP, clicks Join.
4. Joiner UI shows `Connecting… → Downloading world… 12.3 / 48.6 MB → Loading…`.
5. Joiner's Kenshi calls `TitleScreen::importGame` on the freshly-downloaded snapshot. Kenshi loads it the same way it would load any other save.
6. Once the joiner's load completes, the plugin opens the ENet connection and sends a `ConnectRequest` (existing packet). The session is live.

At this point both sides have identical world state. Spec B and onwards handle what happens next.

## Architecture

### Components

```
┌─────────────────────────┐           ┌─────────────────────────┐
│  Host Kenshi (plugin)   │           │  Joiner Kenshi (plugin) │
│                         │           │                         │
│   ├─ save trigger hook  │           │   ├─ Multiplayer menu   │
│   └─ save zip producer ─┼──(IPC)──┐ │   ├─ HTTP client        │
│                         │         │ │   ├─ Zip extractor      │
└─────────────────────────┘         │ │   └─ importGame trigger │
           │ (spawns)                │ │                         │
           ▼                         │ └──────────┬──────────────┘
┌─────────────────────────┐          │            │
│   Server process        │          │            │ HTTP GET /snapshot
│                         │          │            │ (zip, gzip'd)
│   ├─ ENet session mgr   │          │            │
│   │  (existing)         │          ▼            │
│   ├─ snapshot store ────┼─────(zip blob in RAM)│
│   └─ HTTP sidecar ──────┼─────────────────────┘
│       (port+1)          │
└─────────────────────────┘
```

### Processes and ports

- **Host Kenshi**: one process, runs plugin DLL injected by RE_Kenshi.
- **Server**: child process spawned by plugin (existing: `kenshi-mp-server.exe`). Owns ENet on `port` (default 7777) and HTTP sidecar on `port+1` (default 7778).
- **Joiner Kenshi**: one process, plugin DLL. Makes one outbound HTTP request to `http://<host>:<port+1>/snapshot`, then one outbound ENet connection to `<host>:<port>`.

### Components to add

- **Save trigger hook** (plugin / host side): identifies and calls Kenshi's "save now to slot X" function. Blocks until save completes. Added to `core/src/`.
- **Save zip producer** (server side): walks the save slot directory, zips to an in-memory buffer. Added to `server/core/src/snapshot.{h,cpp}`.
- **HTTP sidecar** (server side): embeds `cpp-httplib` (header-only, MIT, already compatible with the MSVC toolchain used for server). Single `GET /snapshot` endpoint. Added to `server/core/src/http_sidecar.{h,cpp}`.
- **IPC host→server snapshot path** (both sides): the plugin needs to hand the saved bytes to the server process. Simplest approach: plugin zips the save itself (plugin lives in the host's Kenshi process, has filesystem access), passes the zip bytes to server via an existing ENet admin message, e.g. `SNAPSHOT_UPLOAD` packet split into chunks. Alternative: shared temp file path. Picked: **plugin zips, uploads via a new ENet packet** — keeps things in-process on the server side.
- **Multiplayer menu rework** (client side): split "Host" and "Join" into two distinct flows. "Host" asks for a save slot and launches server + connects. "Join" asks for IP, downloads snapshot via HTTP, loads it, then connects.
- **HTTP client** (client side): same `cpp-httplib` for uniformity. Single `GET`. Added to `core/src/snapshot_client.{h,cpp}`.
- **Zip extraction** (client side): `miniz` (single-header, public-domain). Added alongside snapshot_client.
- **Programmatic `importGame` trigger**: call into `TitleScreen::importGame` (already exported by KenshiLib) with the freshly-downloaded save slot path.

## Data flow

### 1. Host start

```
Host clicks "Multiplayer → Host" with slot "MyCampaign"
  → plugin launches server process
  → plugin calls save-trigger hook on slot "KMP_<hostid>" (copy of MyCampaign, new slot so we never overwrite the player's solo save)
  → save completes, plugin reads the slot dir
  → plugin zips slot dir → bytes in RAM
  → plugin sends bytes to server over ENet via SNAPSHOT_UPLOAD chunks (reliable channel)
  → server reassembles, stores as snapshot rev 1, starts HTTP sidecar
  → host's plugin now opens its own ENet client connection (it's a player too)
```

### 2. Joiner connect

```
Joiner clicks "Multiplayer → Join", enters IP
  → HTTP GET http://host:port+1/snapshot (with Accept-Encoding: gzip)
     → server streams snapshot blob, response includes X-KMP-Snapshot-Rev: 1
  → client writes bytes to temp file, extracts zip into My Games/Kenshi/save/KMP_<hostid>/
  → client persists snapshot rev 1 alongside the slot (so future reconnects can skip download if unchanged)
  → client calls TitleScreen::importGame on slot "KMP_<hostid>"
  → Kenshi loads the slot (same path as any normal save load)
  → plugin detects load complete (hook on loading finished), opens ENet to host:port, sends ConnectRequest
```

### 3. Reconnect with same snapshot rev

```
Joiner reconnects
  → HTTP HEAD /snapshot (or GET with If-None-Match: <rev>)
  → server responds 304 Not Modified if rev unchanged, 200 + new bytes otherwise
  → if 304: skip download, reuse existing slot dir
  → same load path as first connect
```

## Protocol additions

Add three new packet types:

- `SNAPSHOT_UPLOAD_BEGIN` — host → server. Fields: `total_size`, `rev`, `sha256`. Sent before any chunks.
- `SNAPSHOT_UPLOAD_CHUNK` — host → server. Fields: `offset`, `length`, `bytes[]`. Max chunk size e.g. 64 KB.
- `SNAPSHOT_UPLOAD_END` — host → server. Fields: `rev`. Server verifies sha256, atomically swaps snapshot.

If any chunk is missing or sha mismatch, server drops the in-progress snapshot and logs a warning. Host retries on next auto-save (Spec D) or can be re-triggered manually.

The HTTP sidecar has one endpoint:

- `GET /snapshot` — returns `200 application/zip` with the current snapshot, header `X-KMP-Snapshot-Rev: N`. Returns `503 Service Unavailable` if no snapshot uploaded yet (host hasn't finished first save).
- `GET /snapshot` with `If-None-Match: "<rev>"` — returns `304 Not Modified` if rev matches.

## File layout

Client-side slot naming: `My Games/Kenshi/save/KMP_<hostid>/` where `<hostid>` is `<hostname>_<port>` sanitised to a valid filename. First connect creates the folder; subsequent connects reuse and overwrite.

Server-side: snapshot lives entirely in RAM. Not persisted on the server machine — if the server process dies, next time the host starts, a new snapshot is created from the current save state.

## Error handling

- **HTTP GET fails / times out (network error)**: client UI shows `Failed to download world. Check host address.` User can retry. No partial state written.
- **Zip extraction fails (corrupt or truncated)**: delete the partially-extracted slot dir, UI shows error, user retries.
- **`importGame` fails (Kenshi rejects the slot)**: logged with the save slot path. UI shows `Failed to load world — is your Kenshi version and mod list the same as the host?`. Most likely cause is mod mismatch, hence the hint. Spec N (future) will properly check.
- **HTTP sidecar fails to bind (port in use)**: server logs warning, refuses to host. UI shows `Port <port+1> in use — close whatever's using it or pick a different base port.`
- **Snapshot upload from plugin to server is interrupted (host crash mid-save)**: server keeps the previous snapshot if any, or stays at 503 if this was first. Host re-triggers on next connect.
- **Save slot `KMP_<hostid>` doesn't exist when host tries to create it**: plugin copies the chosen source save folder to `KMP_<hostid>` first, then triggers save. If the copy fails (disk full, permissions), plugin aborts, UI shows error.

## Testing strategy

- **Unit**: zip round-trip (zip then extract, compare byte-for-byte) — mini-golden test with a fixture folder.
- **Unit**: SNAPSHOT_UPLOAD chunking/reassembly — test with a known blob, verify sha.
- **Integration**: spin up server + a test HTTP client, upload a known blob, GET it back, compare.
- **Manual end-to-end** (no way around this for Kenshi-specific stuff):
  1. Host loads a known save, starts server, confirms snapshot uploaded (log check).
  2. Joiner on same machine, different Kenshi install or same via cmdline arg, joins, confirms world loads with same NPCs/buildings/items visible.
  3. Joiner reconnects without host changing anything — confirms HTTP 304 path, no re-download.
  4. Host saves again (via forced trigger for the test) — joiner reconnects, confirms re-download of rev 2.
  5. Corrupt test: truncate the zip in transit (via proxy) — confirm joiner shows error cleanly, doesn't leave half-extracted slot.

## Open questions to resolve during implementation

1. **Which exact Kenshi function to hook for "save now"?** Needs RE work — probably `OptionsWindow::saveButton` or an internal `GameWorld::save(slot)`. If no clean single entry point, we fall back to synthesising keystrokes (F5 quicksave) which is hacky.
2. **Does `TitleScreen::importGame` accept an arbitrary slot path, or does it need the slot already registered in Kenshi's save list?** If the latter, we need to write to whatever index file Kenshi reads.
3. **How does Kenshi signal "load finished"?** Need to find a hook point so the plugin knows when to open the ENet connection. Likely a `GameWorld::loadGameCallback` or similar.

These get resolved in the implementation plan, not this design.

## Rollout

This spec is a single coherent milestone. Ship it when end-to-end manual test #1 passes. Existing F8 connect flow remains as a fallback during development; the new menu flow replaces it once Spec A + C ship.
