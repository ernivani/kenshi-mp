# Spec A.2 — Host snapshot upload

**Status:** Draft
**Date:** 2026-04-21
**Parent spec:** `docs/superpowers/specs/2026-04-21-save-transfer-and-load-design.md`
**Depends on:** Plan A.1 (shipped on main) — server-side `SnapshotStore`, `SnapshotUploadSession`, `HttpSidecar`, `SNAPSHOT_UPLOAD_*` packets.

## Goal

When a player clicks "Host", their Kenshi plugin produces a fresh save of the current world, zips it, uploads it to the embedded server over the existing ENet connection, and makes it available on `GET http://host:port+1/snapshot` for joiners. After this spec ships, a joiner running `curl` against the sidecar gets back a valid zip of the host's actual world state.

This spec closes the "producer" side of the save-transfer pipeline. Joiners downloading and loading it is Plan A.3.

## Non-goals

- Re-uploading the snapshot when the world drifts (Plan D: auto-save scheduling).
- Auto-retry on failure — host reconnect is the only retry path.
- Incremental / delta uploads — MVP does a full snapshot every time.
- Progress bars with percentages during the zip step (just "packaging..." status).
- Compressing at a specific level — miniz default is fine.
- Touching the host's actual in-progress solo save slot. We write to a dedicated `KMP_Session` slot only.

## User-visible flow

1. Player clicks **Host** in the Multiplayer window (existing UI) or via main-menu button.
2. Server process spawns, plugin opens ENet, plugin sends `ConnectRequest`. (all existing)
3. Server accepts → plugin receives `ConnectAccept`. (existing)
4. **New:** plugin triggers `SaveFileSystem::getSingleton()->saveGame("KMP_Session")` and the status bar shows `Hosting: saving world...`.
5. Plugin polls `SaveFileSystem::busy()` each tick. When it returns false, status becomes `Hosting: packaging world...` and a worker thread zips the save folder.
6. When the worker thread finishes, status becomes `Hosting: uploading 0.0 / X.X MB` and updates each tick as chunks go out.
7. When the server ACKs: status becomes `KenshiMP - HOSTING as Player #N  ·  World ready ✓`. Chat shows `World ready — joiners can connect`.
8. Joiner downloading `/snapshot` now gets 200 + the zip.

On any failure, status reads `Hosting: upload failed (<reason>) — joiners can't connect` and stays there until the host reconnects (which re-fires the whole flow).

## Architecture

### Components (all new, all in `core/src/` — plugin side, VS2010 toolchain)

- **`save_trigger.{h,cpp}`** — wraps Kenshi's `SaveFileSystem`. Single entry point `trigger_save(std::string slot_name)` that calls `saveGame()` and transitions internal state. `poll_busy()` returns true while saving. `last_error()` returns a string on failure. No direct ENet or zip dependency.
- **`snapshot_zip.{h,cpp}`** — single entry `std::vector<uint8_t> zip_directory(const std::string& abs_path)`. Walks recursively, deflates, returns blob in memory. Runs on a background thread — MUST NOT call any `kenshi/*` or MyGUI API. Only miniz + stdlib.
- **`snapshot_uploader.{h,cpp}`** — the state machine. Owns a background thread handle for the zip step. Exposes:
  - `start(slot_name)` — begin a new upload.
  - `tick(dt)` — drive the state machine (called from `player_sync_tick`).
  - `state()` / `progress_text()` — for UI.
  - `on_ack(const SnapshotUploadAck&)` — called from the receive path.
- **`deps/miniz/miniz.h` + `miniz.c`** — vendored. Public domain single-header + its matching `.c`. Included in the plugin CMakeLists.

### Integration points (minimal)

- `core/src/ui.cpp`:
  - In `ui_on_connect_accept(player_id)`: if `host_sync_is_host()`, call `snapshot_uploader::start("KMP_Session")`.
  - In `update_status_text()`: when connected as host, append `snapshot_uploader::progress_text()` if any.
- `core/src/player_sync.cpp` tick path: call `snapshot_uploader::tick(dt)` right after `ui_check_hotkey()`.
- `core/src/client.cpp` receive path: dispatch `PacketType::SNAPSHOT_UPLOAD_ACK` to `snapshot_uploader::on_ack(...)`.

Total external-surface changes: three function calls added in three existing files.

## State machine

```
IDLE ──start()──▶ TRIGGER_SAVE
                     │
                     ▼
              (saveGame called, returns bool)
              success? ──no──▶ FAILED("save refused")
                     │yes
                     ▼
                 WAIT_SAVE ──tick: busy()?── yes──▶ (loop, 60s timeout)
                     │ no
                     ▼
                 ZIP_START ── spawn worker ──▶ ZIP_RUNNING
                                                    │
                                                    ▼
                                         (worker thread: zip_directory)
                                                    │
                                                    ▼
                                         tick: atomic done? ── no──▶ (loop)
                                                    │ yes
                                                    ▼
                                               join thread
                                               compute sha256
                                                    │
                                                    ▼
                                               SEND_BEGIN ──▶ SEND_CHUNKS
                                                                  │
                                             (tick: send 1-3 chunks, 60 KB each)
                                                                  │
                                                   offset == total_size?
                                                                  │ yes
                                                                  ▼
                                                              SEND_END
                                                                  │
                                                                  ▼
                                                            AWAIT_ACK (30s timeout)
                                                                  │
                                            on_ack(accepted=1) ──▶ IDLE (success)
                                            on_ack(accepted=0) ──▶ FAILED(code)
                                                 timeout ───────▶ FAILED("no ACK")
```

Every transition logs one line via `KMP_LOG`. FAILED is terminal — `start()` must be called again to retry, which only happens on reconnect for MVP.

## Data layout

- **Source slot path:** `<Documents>/My Games/Kenshi/save/KMP_Session/`.
- `<Documents>` resolved via `SHGetFolderPathW(CSIDL_PERSONAL, ...)` at plugin init and cached.
- Save folder is created by `SaveFileSystem::saveGame("KMP_Session")` if it doesn't exist.
- Zip file itself is compressed by miniz (default deflate level 6). We send the zip bytes over ENet as-is, no additional compression layer (the ENet reliable channel is already framed).
- `upload_id` is a random 32-bit value per upload (so concurrent edge cases would be distinguishable). We only have one at a time in MVP, but the wire already supports it.
- Chunk size is `60 * 1024` bytes (60 KB). Chunks per tick starts at 1; tunable constant `CHUNKS_PER_TICK`.

## Threading invariants

- Main thread (Kenshi render / our per-frame hook): all ENet sends, all Kenshi API calls, all UI writes. Drives the state machine tick.
- Worker thread (spawned once per ZIP step, joined before transition out of ZIP_RUNNING): only filesystem I/O + miniz. Never touches `SaveFileSystem`, `MyGUI`, `ou`, or anything Kenshi. Communicates via `std::atomic<bool>` flag + owned `std::vector<uint8_t>*` returned by `get()`.
- No other threads.

## Error handling

| Failure | Detection | Status text | Recovery |
|---|---|---|---|
| `saveGame()` returned false | return value | `save failed (Kenshi refused)` | host must reconnect |
| `busy()` never flips | 60s timeout in WAIT_SAVE | `save timed out` | reconnect |
| Slot dir missing after save | zip_directory can't open path | `save folder not found` | reconnect |
| zip_directory returned empty | size == 0 | `packaging failed` | reconnect |
| Zip size > 512 MB | check before SEND_BEGIN | `save too large (X MB > 512)` | reconnect with smaller save |
| `client_send_reliable` returns false | return | `send failed` (host likely disconnected) | reconnect auto-triggers flow |
| Server ACK `accepted=0` | on_ack callback | `server rejected upload (<code>)` | reconnect |
| No ACK in 30s after END | AWAIT_ACK timeout | `server didn't confirm upload` | reconnect |

All failures are terminal for the current session. Auto-retry is explicitly deferred.

## Testing

**Unit tests** (run by `make test` alongside Plan A.1 tests):

- `tools/test_snapshot_zip.cpp` — write known files to a temp dir, call `zip_directory`, then unzip the resulting blob with miniz into a second temp dir. Assert every file present, byte-identical, directory structure preserved. Uses `KMP_CHECK`.
- `tools/test_snapshot_uploader.cpp` — construct a `SnapshotUploader` with mock callbacks (save trigger, send-reliable, zip function). Drive the tick loop through:
  - Happy path: mocked save completes → mocked zip returns blob → expected BEGIN+chunks+END sent → inject ACK → state IDLE.
  - saveGame fails: start → tick → state FAILED with right error.
  - Save timeout: mocked busy() stays true → tick until 60s simulated → FAILED.
  - Zip returns empty: FAILED("packaging failed").
  - ACK rejected: inject ACK with accepted=0 → FAILED(code).
  - ACK timeout: never inject → tick simulated 30s → FAILED("no ACK").

  The uploader must be designed to accept injected "clock" + "saveGame" + "zip" + "send" function objects, so tests don't need Kenshi or a real thread. Pattern: `SnapshotUploader(SaveTriggerFn, ZipFn, SendFn, ClockFn)`.

**Manual integration (can't be automated):**

- Small save: `make deploy`, launch Kenshi on a new-game save (~5 MB), click Host, watch status bar through phases. `curl http://127.0.0.1:7778/snapshot -o out.zip` → `unzip -l out.zip` should list the KMP_Session files.
- Large save: same with a 50+ MB mid-game save. Zip step visible through "packaging world..." status for a couple of seconds (the UI freeze is acceptable for MVP; backgrounding zip is the point of the worker thread, so this shouldn't actually freeze).
- Failure path: temporarily set slot path to nonexistent → observe FAILED state + status text.

## Open questions (resolved during implementation)

1. Does `SaveFileSystem::getSingleton()` return non-null at the point where we'd call it? Likely yes (singleton is alive once a save exists), but needs verification. Fallback: poll for non-null before triggering.
2. What exact return-behaviour does `saveGame` have when the slot path is invalid? The header says `bool` but doesn't describe the error cases. Test by feeding an invalid path early in implementation.
3. miniz' recursive directory walking — does it have a helper, or do we walk with `FindFirstFileW` ourselves? Likely the latter; trivial.

## Rollout

Ships when:
- All unit tests pass via `make test`.
- Manual integration test 1 (small save) passes.
- `make deploy` + Kenshi launch + click Host → within 20s, `curl /snapshot` returns 200 + a zip that matches the KMP_Session folder on disk.

Then Plan A.3 can start: joiner-side download + load, which closes the end-to-end loop.
