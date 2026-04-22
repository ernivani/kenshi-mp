# Spec A.4 — Joiner runtime

**Status:** Draft
**Date:** 2026-04-22
**Parent:** `docs/superpowers/specs/2026-04-21-save-transfer-and-load-design.md`
**Depends on:** Plans A.1 (server plumbing) + A.2 (host upload) + A.3 (server browser UI), all merged to main.

## Goal

Wire the server browser's Join button to a real pipeline: download the host's snapshot over HTTP, extract to a local save slot, load it via `SaveManager::loadGame`, open the ENet connection with the password, and auto-transition into the loaded world once the server accepts. The existing Connecting modal animates through the stages (Downloading → Extracting → Loading → Connecting). Cancel aborts cleanly at any stage where aborting is possible.

After this spec ships, the end-to-end Minecraft-style flow works: click Join → see progress → land in the host's world. That closes the A-series.

## Non-goals

- Persistent per-player character state on the server (Plan B).
- Auto-save on the host, periodic re-upload, "host saved — re-download?" UX (Plan D).
- Runtime authority beyond existing A.1 sync (Plan E).
- Mod-list compatibility check; if the joiner is missing a mod Kenshi may crash on load — we surface that as a generic "Kenshi refused to load the world" for now.
- Incremental/delta downloads. MVP re-downloads the full snapshot whenever a host's `X-KMP-Snapshot-Rev` changes.
- Save slot cleanup (the `KMP_<hostid>/` folders accumulate over time). Future polish.

## User-visible flow

1. Player opens Multiplayer browser, selects or direct-connects to a server.
2. Click **Join** (or double-click a row with a successful ping).
3. Connecting modal shows `Downloading world.` / `..` / `...` with `12.3 / 48.6 MB` on the second line. Cancel button available.
4. Once 100%: `Extracting.` / `..` / `...` (usually under 1 s).
5. Then `Loading world.` / `..` / `...` for a few seconds while Kenshi reads the save.
6. Then `Connecting.` / `..` / `...` while ENet handshake completes.
7. On `CONNECT_ACCEPT`: modal vanishes, browser vanishes, the joiner is standing in the host's world with the existing per-frame sync (NPCs, buildings, positions) running. Total wall time: ~30 s for a 50 MB save.

On any error at any stage: modal switches to `Error at <stage>: <message>` with Retry (if applicable) and Close buttons. Close returns to the browser so the user can edit the entry or pick another server.

## Architecture

### New plugin-side files

- `core/src/snapshot_client.h` / `.cpp` — WinHTTP wrapper. Blocks in a worker; progress via callback. One public function `download_snapshot_blocking(host, port, out_path, progress_cb)`.
- `core/src/snapshot_extract.h` / `.cpp` — miniz wrapper. `extract_zip_to_dir(zip_path, dst_dir)` returns bool. Synchronous.
- `core/src/load_trigger.h` / `.cpp` — wraps `SaveManager::loadGame(location, slot)` + `SaveFileSystem::busy()` polling. Mirror of `save_trigger` in reverse. Also knows the save root (re-uses `save_trigger_resolve_slot_path`).
- `core/src/joiner_runtime.h` / `.cpp` — the DI state machine that orchestrates the pipeline.
- `core/src/joiner_runtime_glue.h` / `.cpp` — real Kenshi/ENet/thread bindings mirroring the A.2 `snapshot_uploader_glue` pattern.

### Protocol additions

- `common/include/packets.h`: `ConnectRequest` gets `char password[64]`. Default empty. Existing ctor `std::memset`s the struct, so older clients will send zero-bytes.
- `server/core/src/session.cpp`: on `CONNECT_REQUEST`, if `s_server_config->password` non-empty AND `pkt.password != s_server_config->password`, respond `ConnectReject("wrong password")` and disconnect.

### Integration points

- `core/src/server_browser.cpp`: `on_join` and the double-click path call `joiner_runtime_start(entry)` instead of the placeholder log + modal. The existing Connecting modal (and its cancel button) are driven by `joiner_runtime` state.
- `core/src/player_sync.cpp`: on `CONNECT_ACCEPT`, call `joiner_runtime_on_connect_accept(player_id)` which transitions the runtime to DONE. Existing body of the CONNECT_ACCEPT handler (hide local NPCs/buildings, send combat stats, `ui_on_connect_accept`) stays — `joiner_runtime` runs after.
- `core/src/player_sync.cpp`: on `CONNECT_REJECT`, call `joiner_runtime_on_connect_reject(reason)`.
- `core/src/plugin.cpp`: `joiner_runtime_init/shutdown/tick` calls next to `snapshot_uploader_glue_*`.

## State machine

```
IDLE ──start(entry)──▶ DOWNLOADING
                          │
                      (HTTP GET /snapshot with progress callback)
                          │
                        done ──▶ EXTRACTING
                          │
                      (miniz extract to <save_root>/KMP_<hostid>/)
                          │
                        done ──▶ LOAD_TRIGGER
                          │
                      SaveManager::loadGame(<save_root>, "KMP_<hostid>")
                          │
                        returned ──▶ LOAD_WAIT
                          │
                      (poll SaveFileSystem::busy() each tick, 4s grace)
                          │
                        !busy ──▶ ENET_CONNECT
                          │
                      client_connect(host, port) + send ConnectRequest{password}
                          │
                        sent ──▶ AWAIT_ACCEPT
                          │
            ┌─── CONNECT_ACCEPT ───▶ DONE
            │
            ├─── CONNECT_REJECT ───▶ FAILED("wrong password" | "<reason>")
            │
            └─── 30s timeout ──────▶ FAILED("server didn't respond")
```

Each non-terminal state also handles `cancel()`:
- DOWNLOADING: abort the WinHTTP handle, delete partial zip, → CANCELLED.
- EXTRACTING: join the worker (I/O short), delete partial slot dir, → CANCELLED.
- LOAD_* : cannot abort Kenshi's load cleanly. Modal shows `Cancelling...`, pipeline transitions to CANCELLED on its own once load finishes; the loaded world stays visible but ENet never connects. User can Esc back to title.
- ENET_CONNECT / AWAIT_ACCEPT: `client_disconnect()`, same local-only state as above.

Terminal states (DONE, CANCELLED, FAILED) cannot transition further without a fresh `start()`.

## Host ID derivation

`host_id = first_8_hex(sha256("<address>:<port>"))`.

- Stable for a given server (same ID between sessions).
- Different for different servers (joining another host gives a new local slot).
- Uses picosha2 (already vendored).

Slot path: `<AppData>/Local/kenshi/save/KMP_<host_id>/` (Kenshi resolves this via `SaveManager::userSavePath` — we re-use `save_trigger_resolve_slot_path`).

## DI shape (unit-testable without real threads / HTTP / Kenshi)

```cpp
class JoinerRuntime {
public:
    struct Deps {
        std::function<void(const std::string& host, uint16_t port,
                           const std::string& out_path)>  start_download;
        std::function<bool(uint64_t& bytes_done, uint64_t& bytes_total)> poll_download;
        std::function<void()>                                            cancel_download;

        std::function<void(const std::string& zip_path,
                           const std::string& dst_dir)>                  start_extract;
        std::function<bool(bool& ok)>                                    poll_extract;

        std::function<bool(const std::string& location,
                           const std::string& slot)>                     trigger_load;
        std::function<bool()>                                            is_load_busy;

        std::function<bool(const std::string& host, uint16_t port)>      connect_enet;
        std::function<bool(const std::string& password)>                 send_connect_request;
        std::function<void()>                                            disconnect_enet;

        std::function<float()>                                           now_seconds;
        std::function<std::string(const std::string&)>                   resolve_slot_path;
    };

    enum class Stage { Downloading, Extracting, Loading, Connecting };
    struct State { enum E { Idle, Downloading, Extracting,
                            LoadTrigger, LoadWait,
                            EnetConnect, AwaitAccept,
                            Done, Cancelled, Failed }; };

    explicit JoinerRuntime(Deps deps);
    void start(const ServerEntry& entry);
    void cancel();
    void tick(float dt);
    void on_connect_accept(uint32_t player_id);
    void on_connect_reject(const std::string& reason);

    State::E state() const;
    std::string progress_text() const;
    std::string last_error() const;
};
```

## Error handling

| Stage | Failure | Modal message | Retry? |
|---|---|---|---|
| DOWNLOADING | WinHTTP connect error | "Cannot reach server" | yes |
| DOWNLOADING | HTTP 503 | "Host hasn't uploaded the world yet" | yes (re-ping first) |
| DOWNLOADING | HTTP 4xx/5xx | `Server error HTTP <code>` | yes |
| DOWNLOADING | 120 s timeout | "Download timed out" | yes |
| EXTRACTING | miniz error | "Extracted world is corrupt" | no (re-download) |
| EXTRACTING | fs write error | "Cannot write to save folder" | no |
| LOAD_TRIGGER | `loadGame` returns < 0 | "Kenshi refused to load the world" | no |
| LOAD_WAIT | `busy()` stuck > 120 s | "Load timed out" | no |
| ENET_CONNECT | `client_connect` returns false | "Cannot open connection" | yes |
| AWAIT_ACCEPT | 30 s without ACCEPT/REJECT | "Server didn't respond" | yes |
| AWAIT_ACCEPT | `CONNECT_REJECT` "wrong password" | "Wrong password" | no (edit entry + retry) |
| AWAIT_ACCEPT | `CONNECT_REJECT` other | `Rejected: <reason>` | no |

The Connecting modal, in FAILED state, switches its layout to show `Error at <stage>: <message>` + Retry button (shown iff retryable) + Close button (returns to browser).

## Testing

### Unit — `tools/test_joiner_runtime.cpp`

Mock all Deps. Cases:
- **happy_path** — drive through every state, end at Done.
- **download_error_times_out** — poll_download never returns done, clock advances > 120 s → Failed.
- **extract_error** — poll_extract returns `(done=true, ok=false)` → Failed.
- **load_refused** — trigger_load returns false → Failed.
- **load_timeout** — is_load_busy stays true 120 s → Failed.
- **connect_enet_fails** — connect_enet returns false → Failed.
- **accept_timeout** — neither on_connect_accept nor reject for 30 s → Failed.
- **reject_password** — inject `on_connect_reject("wrong password")` → Failed with retry=false.
- **cancel_during_download** — cancel() mid-download calls cancel_download, state → Cancelled.
- **cancel_during_load** — cancel during LoadWait → state stays Cancelling, transitions Cancelled once `is_load_busy` returns false.

All tests run synchronously, no real threads, no real HTTP, no real Kenshi.

### Manual integration

1. Host + joiner on same PC. Host clicks Host, world uploads. Joiner opens browser, Adds server `127.0.0.1`, sees `0/32  <n>ms`, clicks Join. Modal progresses through phases. Within ~30 s the joiner is standing in the host's world with the host's player visible.
2. Host edits `server_config.json` to `"password": "hunter2"`, restarts. Joiner joins with empty password → "Wrong password". Joiner edits entry, saves password, retries → OK.
3. During the Download phase, click Cancel → modal closes, browser re-enabled, no partial zip or slot left on disk.

### Done definition for A.4

- All 10 unit tests pass via `make test`.
- Manual test 1 succeeds end-to-end on two local Kenshi instances.
- Manual tests 2 and 3 behave as described.
- On a fresh Kenshi boot followed by Join + Success, `<AppData>/Local/kenshi/save/KMP_<hostid>/` exists and contains the expected save files (quick.save, platoon/, zone/).

## Open implementation issues

1. **WinHTTP progress callbacks** — WinHTTP supports status callbacks via `WinHttpSetStatusCallback`; we set `WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE` and push bytes-done into an atomic counter. No third-party dep.
2. **Auth-less snapshot endpoint** — the HTTP sidecar serves `/snapshot` without any auth. If someone scans the LAN they can download a world snapshot. Acceptable for v1 (password gates the ENet connection, which is where play state lives). Future plan: short-lived token handed out on `SERVER_INFO_REPLY` that the HTTP GET must include.
3. **Sub-120s load time on big saves** — Kenshi loads large saves in 5-30 s on SSD. 120 s timeout is generous; future versions can tune.

## Rollout

Ships when done-definition passes. Merges to main. At that point the A-series (persistent multiplayer foundation) is closed and we're ready for B (faction/character lifecycle) or D (auto-save/session lifecycle).
