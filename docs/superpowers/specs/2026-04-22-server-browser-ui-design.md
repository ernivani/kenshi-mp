# Spec A.3 — Server browser UI

**Status:** Draft
**Date:** 2026-04-22
**Parent spec:** `docs/superpowers/specs/2026-04-21-save-transfer-and-load-design.md`
**Depends on:** Plan A.1 (server plumbing) + Plan A.2 (host snapshot upload), both merged to main.

## Goal

Replace the F8 Host/Join dialog with a Minecraft-style Multiplayer Servers screen. The main-menu Multiplayer button opens a modal listing saved servers; each row shows live ping + player count + description pulled from the server via an unauth `SERVER_INFO_REQUEST` packet. Add / Edit / Remove / Direct Connect / Join buttons. Entries persist to `servers.json` in `My Documents\My Games\Kenshi\KenshiMP\`.

After this spec ships, the user can manage a real server list. The Join button logs but does not yet download/load — that's Plan A.4 (joiner runtime).

## Non-goals (explicitly deferred)

- Actual download + extract + load on Join (→ A.4).
- Password gate at connect time (→ A.4, where Join is wired).
- LAN auto-discovery (future).
- Steam Workshop / forum server listings (future).
- Password encryption in `servers.json` (stored plaintext per user decision).
- A second-screen "browse public servers" feed (future).

## User-visible flow

1. Title screen → click **Multiplayer** (existing button from A.1).
2. Modal "Multiplayer Servers" appears, 600×460, centered.
3. First time: empty list + prominent **Add Server** button.
4. Click **Add Server** → sub-modal with Name / Address / Port / Password fields → OK → entry appears in the list, instantly pinged; within ~1s the row shows `12/32  45ms  "Surviving in the Holy Nation"` or `— offline` if unreachable.
5. Select a row → **Edit** / **Remove** / **Join** become enabled.
6. **Direct Connect** button → one-shot dialog for Address/Port/Password that connects without saving.
7. **Refresh** button → re-pings all rows.
8. Close window (X or Esc) → pings aborted, selection forgotten, list persisted.

## Architecture

### New plugin-side files

- `core/src/server_browser.{h,cpp}` — MyGUI modal window, list widget, sub-dialogs. Hooks the existing main-menu Multiplayer button (currently `on_mp_menu_clicked` → opens F8 dialog; replaced to open this browser).
- `core/src/server_list.{h,cpp}` — pure data layer. Load/save `servers.json`. In-memory `std::vector<ServerEntry>`.
- `core/src/server_pinger.{h,cpp}` — sends `SERVER_INFO_REQUEST` to each entry when browser opens, keyed by `entry.id`. Owns a transient ENet host separate from the game's ENet client (so pinging doesn't interfere with an in-progress multiplayer session).

### New common-side additions

- `common/include/packets.h`:
  - `SERVER_INFO_REQUEST` (0xB0) — `{header, nonce}`
  - `SERVER_INFO_REPLY` (0xB1) — `{header, nonce, player_count, max_players, protocol_version, password_required, _pad[2], description[128]}`

### New server-side additions

- `server/core/src/session.cpp` (or a new `server_info.cpp`) — handle `SERVER_INFO_REQUEST` BEFORE the `CONNECT_REQUEST` gate. Respond with populated reply using live state.
- `server/core/src/server_config.h` — new string fields `description` (user-facing blurb) and `password` (empty = no password gate). Both persisted in `server/config.json`. Existing `max_players` reused.

### Touch points on existing files

- `core/src/ui.cpp`: `on_mp_menu_clicked` calls into `server_browser_open()` instead of showing the F8 window. The F8 window becomes dead code for now but stays in the source (A.4 reuses pieces of it, then we decide whether to delete).
- `core/src/player_sync.cpp`: add `server_browser_tick(dt)` into the tick call chain so the pinger's ENet host gets serviced each frame while the browser is open.
- `core/src/plugin.cpp`: init `server_list` (read file on first browser open, not at plugin startup — keeps startup path minimal).

## Data layer

### File location

`<Documents>\My Games\Kenshi\KenshiMP\servers.json` (sibling of Kenshi's save root). Plugin creates the `KenshiMP\` subdir on first write if missing. Uses `SHGetFolderPathW(CSIDL_PERSONAL)` same as `save_trigger.cpp`.

### Schema (version 1)

```json
{
  "version": 1,
  "servers": [
    {
      "id": "a1b2c3d4",
      "name": "Bob's Kenshi world",
      "address": "1.2.3.4",
      "port": 7777,
      "password": "",
      "last_joined_ms": 0
    }
  ]
}
```

- `id` — 8-hex-char random, generated once per entry; used as the key for ping results and row identity across refresh cycles.
- `name`, `address`, `port`, `password` — direct from the Add/Edit dialog.
- `last_joined_ms` — unix ms of the last successful join. Written by the (future) Join handler in A.4. Used to sort "most recently played" to the top on browser open.

### API (`server_list.h`)

```cpp
struct ServerEntry {
    std::string id;
    std::string name;
    std::string address;
    uint16_t    port;
    std::string password;
    uint64_t    last_joined_ms;
};

bool        server_list_load(std::vector<ServerEntry>& out);
bool        server_list_save(const std::vector<ServerEntry>& in);
std::string server_list_new_id();
```

`load` returns false if file missing OR parse fails (caller starts with empty list; corrupt files get renamed to `servers.json.corrupt-<ts>` by the implementation so they're not silently overwritten). `save` attempts atomic replace via write-to-tmp + rename.

### JSON handling

`nlohmann/json` isn't usable from the plugin (VS2010 v100 STL has partial C++11). Hand-roll a minimal parser + writer in `server_list.cpp` (~100 lines; flat schema, no nested objects beyond the array). Strict-mode: anything unexpected = parse fail.

## UI layout

### Main window

`Kenshi_WindowCX` skin, 600×460, centered on the TitleScreen's widget. Caption "Multiplayer Servers". X button closes.

```
┌────────────────────────────────────────────────────────┐
│  Multiplayer Servers                               [X] │
├────────────────────────────────────────────────────────┤
│  [ Refresh ]                                           │
├────────────────────────────────────────────────────────┤
│  ┌──────────────────────────────────────────────────┐  │
│  │ Bob's Kenshi world          12/32   45ms   ✓     │  │  ← selected
│  │ 1.2.3.4:7777  "Surviving in the Holy Nation"    │  │
│  ├──────────────────────────────────────────────────┤  │
│  │ Alice's LAN                 —       — offline    │  │
│  │ 192.168.1.5:7777                                 │  │
│  └──────────────────────────────────────────────────┘  │  ← scrollable
├────────────────────────────────────────────────────────┤
│ [ Direct Connect ]  [ Add ]  [ Edit ]  [ Remove ] [ Join ] │
└────────────────────────────────────────────────────────┘
```

### Row widget

Each row is a `MyGUI::Widget` subclass (or a plain widget with two TextBox children). Two visual lines:
- Line 1: `<name>` left-aligned + `<players>` + `<ping>` + `<status icon>` right-aligned.
- Line 2 (smaller font, dimmer color): `<ip>:<port>` + quoted `<description>`.

Row height ~44 px. Rows stack vertically in a scrollable container. Hover tints the background; click selects (fills with highlight color). Selecting a different row deselects the prior one.

### Sub-dialogs

- **Add / Edit** (320×260): labeled EditBoxes for Name, Address, Port, Password. OK / Cancel. Edit pre-fills. OK writes through `server_list_save`, re-pings the new/modified entry, re-renders the row.
- **Direct Connect** (320×160): Address, Port, Password EditBoxes. OK triggers Join behavior without saving (log in A.3 scope, actual join in A.4). Cancel closes.
- **Remove confirm** (280×100): "Remove 'X'?" OK / Cancel. OK removes the entry + saves.

### Button states

| Button | Enabled when |
|---|---|
| Refresh | Always |
| Direct Connect | Always |
| Add | Always |
| Edit | A row is selected |
| Remove | A row is selected |
| Join | A row is selected AND its ping succeeded AND protocol_version matches |

## Ping protocol

### Request/reply structs

See "Architecture → New common-side additions" above.

**Wire layout of `ServerInfoReply`**: 2-byte PacketHeader + 4-byte nonce + 2-byte player_count + 2-byte max_players + 1-byte protocol_version + 1-byte password_required + 2-byte padding + 128-byte description. Total 142 bytes. Fits comfortably in a single ENet packet.

### Client ping flow (per entry, parallel)

1. Resolve hostname → IP via `enet_address_set_host`. On failure: mark row `— DNS error`. Done.
2. `enet_host_connect` on a transient ENet host (separate from game's). Start 2s timeout.
3. Poll events. On `ENET_EVENT_TYPE_CONNECT`: record `send_ms = now_ms`, send `SERVER_INFO_REQUEST{nonce = random32}` reliable. Start 2s post-connect timeout.
4. On `ENET_EVENT_TYPE_RECEIVE` with `SERVER_INFO_REPLY` and matching nonce: compute `rtt_ms = now_ms - send_ms`. Store `{ping=rtt_ms, player_count, max_players, description, password_required, protocol_version}` in the row state. Disconnect.
5. On timeout (either stage): mark row `— offline` / `— no reply`.
6. On mismatched protocol_version: mark row `— version mismatch` in red. Join stays disabled.

All pings share one transient ENet host, serviced in `server_browser_tick()` while the window is open. Aborted on window close (destroy the host, discard pending).

### Server side

Add a case in the packet dispatcher for `SERVER_INFO_REQUEST` BEFORE the existing `CONNECT_REQUEST` case. Build and send `SERVER_INFO_REPLY` with live data:
- `nonce` echoed from request.
- `player_count = s_sessions.size()`.
- `max_players = s_config.max_players`.
- `protocol_version = PROTOCOL_VERSION`.
- `password_required = !s_config.password.empty() ? 1 : 0`.
- `description = s_config.description` (truncated to 127 chars + null).

No session setup. Peer stays in the default pre-auth state. Client disconnect afterward is normal.

### Server config additions (`server/core/src/server_config.h/cpp`)

Add two string fields to `ServerConfig`:
- `description` — default "" (empty).
- `password` — default "" (empty, no password required).

Both load from `server/config.json` with safe defaults if missing. Admin GUI gets a later update to expose them (out of scope for A.3 — user edits config.json directly for now).

## Error handling

| Failure | Detection | Behavior |
|---|---|---|
| `servers.json` missing | fopen fails on read | Empty list, save on first Add writes new file |
| `servers.json` parse error | hand-roll parser hits bad token | Log warning, rename bad file to `servers.json.corrupt-<ts>`, start empty |
| `servers.json` save fails | tmp-write fails OR rename fails | Modal error toast "Failed to save server list"; in-memory list remains |
| DNS resolve fails | `enet_address_set_host` ≠ 0 | Row `— DNS error` |
| Connect timeout (2s) | no CONNECT event | Row `— offline` |
| No reply after connect | 2s post-connect timeout | Row `— no reply` |
| protocol_version mismatch | REPLY field vs PROTOCOL_VERSION | Row `— version N vs ours M` red; Join disabled |
| Add dialog: empty name | on OK click | Inline red validation text, don't close |
| Add dialog: invalid IP/host | `enet_address_set_host` preview call on OK | Same |
| Add dialog: port out of range | parse to int, check 1..65535 | Same |
| Add dialog: duplicate entry (same address+port) | scan list on OK | Inline warning, offer "Overwrite?" via a second confirm |
| Window closed during ping | `server_browser_close()` called while host has pending pings | Destroy transient ENet host (aborts all), drop pending replies, clear row state |

## Testing

### Unit (`make test`)

- `tools/test_server_list.cpp` — save a known-content `std::vector<ServerEntry>` to a temp file, load it back, assert field-by-field equality. Also test: missing file → empty list + false return. Corrupt file (hand-written garbage) → false return + file renamed to `.corrupt-<ts>`.
- `tools/test_server_info_packets.cpp` — pack/unpack round-trip for `ServerInfoRequest` and `ServerInfoReply`. Verify the description char array is preserved byte-for-byte and null-terminated.
- `tools/test_server_pinger.cpp` — DI-style with injected ENet and clock mocks. Drive the pinger through: successful reply, connect timeout, post-connect reply timeout, nonce mismatch (ignored), version mismatch (recorded). No real threads, no real sockets.

### Manual integration

1. `make deploy`, launch Kenshi. Click Multiplayer button on title screen. Empty browser modal appears.
2. Click Add Server, fill name/address/port, OK. Row appears; within 2s shows `— offline` (no server running).
3. In another terminal: `./build_server/bin/Release/kenshi-mp-server-headless.exe`. Back in-game: click Refresh. Row now shows `0/16  <n>ms  ""`. (Empty description until config.json sets one.)
4. Edit `server/config.json` adding `"description": "Test server"` + `"password": "hunter2"`, restart server, Refresh browser. Row now shows the description + a 🔒 icon.
5. Close Kenshi. Reopen `servers.json` on disk: confirms the entry persists.
6. Click Join on a populated row: window closes + `[KenshiMP] Join clicked: <name> @ <ip>:<port>` appears in `kenshimp_1.log`. (A.4 will replace this with the real pipeline.)

### Done definition for A.3

- All three unit test suites pass via `make test`.
- Manual integration steps 1–6 pass on a fresh Kenshi launch.
- `servers.json` written in valid JSON; a second process can read and parse it.

## Open questions

1. **Multi-row ping concurrency**: the design says "one ENet host, poll all pings in parallel". ENet supports many outgoing connections on one host, but the handshake machinery doesn't love 10+ simultaneous connects. If we see flakiness, cap to 3-at-a-time with a queue. Defer to implementation if it becomes a problem.
2. **Description truncation vs word-break in the UI**: 128 bytes fits ~60-80 rendered chars depending on font. Long descriptions get cropped by MyGUI automatically; that's fine for MVP.
3. **Is there a ServerConfig already?** yes — `server/core/src/server_config.{h,cpp}` — the spec assumes we extend it.

## Rollout

Ships when done-definition passes. Merges to main. Plan A.4 follows immediately: wire the Join button to HTTP-download → extract → `SaveManager::loadGame` → ENet handshake (with the password field, using the row's stored password).
