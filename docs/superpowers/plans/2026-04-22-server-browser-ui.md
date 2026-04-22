# Server Browser UI (Plan A.3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the F8 Host/Join dialog with a Minecraft-style Multiplayer Servers modal opened from the main-menu button. Saved servers persist to `servers.json`; rows display live ping + player count + description pulled via a new unauth `SERVER_INFO_REQUEST` packet. Join button is a stub that logs (Plan A.4 wires the real flow).

**Architecture:** Three new plugin files (`server_list`, `server_pinger`, `server_browser`) with no cross-coupling besides data flow — list feeds the pinger, pinger feeds the browser's row state. Two new packet types shared between plugin and server. Server-side gets a pre-handshake handler for `SERVER_INFO_REQUEST` and two new config fields (`description`, `password`).

**Tech Stack:** C++11 (plugin v100 toolset) + C++17 (server + tests v143 toolset). MyGUI for UI. ENet for the ping transport. Hand-rolled flat JSON parser on the plugin side (can't link `nlohmann/json` through v100's partial C++11 STL).

**Parent spec:** `docs/superpowers/specs/2026-04-22-server-browser-ui-design.md`.

**Depends on:** Plans A.1 + A.2 (both merged to main; provides packet pattern, `SnapshotUploader` DI style, UI widget patterns, Makefile targets `make core` / `make test` / `make deploy`).

---

## File Structure

**New files:**
- `common/include/packets.h` — add `SERVER_INFO_REQUEST / REPLY` structs (modify)
- `server/core/src/server_config.h` / `.cpp` — add `description`, `password` fields (modify)
- `server/core/src/session.cpp` — handle `SERVER_INFO_REQUEST` before CONNECT_REQUEST gate (modify)
- `core/src/server_list.h` / `.cpp` — JSON-backed persistent list of `ServerEntry`
- `core/src/server_pinger.h` / `.cpp` — DI state machine, one per in-progress batch
- `core/src/server_browser.h` / `.cpp` — MyGUI modal window + Add/Edit/Direct/Remove sub-dialogs
- `core/src/ui.cpp` — replace `on_mp_menu_clicked` body with `server_browser_open()` (modify)
- `core/src/player_sync.cpp` — tick `server_browser_tick(dt)` (modify)
- `core/src/plugin.cpp` — forward-decl + init (modify, minimal)
- `core/CMakeLists.txt` — register 3 new sources (modify)
- `tools/test_server_info_packets.cpp` — round-trip test
- `tools/test_server_list.cpp` — save/load/corrupt-file test
- `tools/test_server_pinger.cpp` — DI-driven state-machine test
- `tools/test_server_info_e2e.cpp` — live-server ping integration test
- `tools/CMakeLists.txt` — 4 new test targets (modify)
- `Makefile` — extend `SNAPSHOT_UNIT_TESTS` and add `test_server_info_e2e` to e2e set (modify)

**Responsibility split:**
- `server_list` knows JSON + filesystem + nothing else.
- `server_pinger` knows ENet + packets + nothing about UI or persistence.
- `server_browser` knows MyGUI + calls into the other two. No ENet, no JSON directly.
- Plugin wiring (`ui.cpp`, `player_sync.cpp`) is a single shim: call `server_browser_open()` on click and `server_browser_tick(dt)` every frame.

---

## Task 1: SERVER_INFO_REQUEST / REPLY packet types

**Files:**
- Modify: `common/include/packets.h`
- Create: `tools/test_server_info_packets.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `Makefile` (extend `SNAPSHOT_UNIT_TESTS`)

Two new fixed-layout packet structs + the `PacketType` constants. Pure wire additions, no behavior yet.

- [ ] **Step 1: Write failing test**

Create `tools/test_server_info_packets.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <cstring>
#include "packets.h"
#include "serialization.h"

using namespace kmp;

static void test_request_roundtrip() {
    ServerInfoRequest orig;
    orig.nonce = 0xDEADBEEF;

    auto buf = pack(orig);
    ServerInfoRequest got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(got.nonce == 0xDEADBEEF);
    KMP_CHECK(got.header.type == PacketType::SERVER_INFO_REQUEST);
    printf("test_request_roundtrip OK\n");
}

static void test_reply_roundtrip() {
    ServerInfoReply orig;
    orig.nonce             = 42;
    orig.player_count      = 3;
    orig.max_players       = 16;
    orig.protocol_version  = PROTOCOL_VERSION;
    orig.password_required = 1;
    std::strncpy(orig.description, "Test server description with punctuation! 1234",
                 sizeof(orig.description) - 1);
    orig.description[sizeof(orig.description) - 1] = '\0';

    auto buf = pack(orig);
    ServerInfoReply got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(got.nonce             == 42);
    KMP_CHECK(got.player_count      == 3);
    KMP_CHECK(got.max_players       == 16);
    KMP_CHECK(got.protocol_version  == PROTOCOL_VERSION);
    KMP_CHECK(got.password_required == 1);
    KMP_CHECK(std::strcmp(got.description,
        "Test server description with punctuation! 1234") == 0);
    printf("test_reply_roundtrip OK\n");
}

int main() {
    test_request_roundtrip();
    test_reply_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt` (match pattern of existing test-* targets):

```cmake
add_executable(test-server-info-packets test_server_info_packets.cpp)
target_link_libraries(test-server-info-packets PRIVATE kenshi-mp-common)
target_compile_features(test-server-info-packets PRIVATE cxx_std_17)
```

Extend `Makefile` `SNAPSHOT_UNIT_TESTS`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip test-snapshot-uploader \
                       test-server-info-packets
```

- [ ] **Step 2: Verify it fails**

```bash
make test 2>&1 | tail -10
```

Expected: compile error `ServerInfoRequest` not declared.

- [ ] **Step 3: Add packet types to `common/include/packets.h`**

Inside `namespace PacketType`, after `SNAPSHOT_UPLOAD_ACK = 0xA3` (introduced by A.1), add:

```cpp
    // Server browser: unauth ping. Client sends REQUEST to a server it's
    // never connected to; server responds with REPLY before CONNECT_REQUEST
    // gate. See docs/superpowers/specs/2026-04-22-server-browser-ui-design.md
    static const uint8_t SERVER_INFO_REQUEST = 0xB0;
    static const uint8_t SERVER_INFO_REPLY   = 0xB1;
```

At the bottom of the file, before the closing `} // namespace kmp` and the `#pragma pack(pop)` (if present — check; A.1 added structs just inside the pragma), add:

```cpp
struct ServerInfoRequest {
    PacketHeader header;
    uint32_t     nonce;

    ServerInfoRequest() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SERVER_INFO_REQUEST;
    }
};

struct ServerInfoReply {
    PacketHeader header;
    uint32_t     nonce;
    uint16_t     player_count;
    uint16_t     max_players;
    uint8_t      protocol_version;
    uint8_t      password_required;  // 0 or 1
    uint8_t      _pad[2];
    char         description[128];   // null-terminated, plain ASCII or UTF-8

    ServerInfoReply() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SERVER_INFO_REPLY;
    }
};
```

- [ ] **Step 4: Run test**

```bash
make test 2>&1 | tail -15
```

Expected:
```
--- test-server-info-packets ---
test_request_roundtrip OK
test_reply_roundtrip OK
ALL PASS
```

- [ ] **Step 5: Commit**

```bash
git add common/include/packets.h tools/test_server_info_packets.cpp \
        tools/CMakeLists.txt Makefile
git commit -m "feat(common): add SERVER_INFO_REQUEST/REPLY packet types"
```

---

## Task 2: Extend `ServerConfig` with description + password

**Files:**
- Modify: `server/core/src/server_config.h`
- Modify: `server/core/src/server_config.cpp`

No test target — the config loader is simple enough that existing server boot exercises it. We verify by rebuilding + booting the headless server with a hand-edited config.json.

- [ ] **Step 1: Add fields to `server_config.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "protocol.h"

namespace kmp {

struct ServerConfig {
    uint16_t    port          = DEFAULT_PORT;
    uint32_t    max_players   = MAX_PLAYERS;
    std::string server_name   = "KenshiMP Server";
    float       view_distance = 5000.0f;
    std::string description;         // advertised in SERVER_INFO_REPLY; "" means no description
    std::string password;            // "" means no password required
};

ServerConfig load_config(const std::string& path);
bool         save_config(const std::string& path, const ServerConfig& cfg);

} // namespace kmp
```

- [ ] **Step 2: Parse new fields in `server_config.cpp::load_config`**

Find the block that reads `if (j.contains("..."))` entries. Append:

```cpp
        if (j.contains("description")) cfg.description = j["description"].get<std::string>();
        if (j.contains("password"))    cfg.password    = j["password"].get<std::string>();
```

- [ ] **Step 3: Write new fields in `save_config`**

Find the block in the same file that builds the JSON for save. Append:

```cpp
    j["description"] = cfg.description;
    j["password"]    = cfg.password;
```

(If `save_config` doesn't already exist in the file, ignore — only `load_config` is critical for this task. Existing server.cfg is hand-edited; the save path is a future admin-GUI concern.)

- [ ] **Step 4: Build**

```bash
make server-core 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add server/core/src/server_config.h server/core/src/server_config.cpp
git commit -m "feat(server): ServerConfig gains description + password fields"
```

---

## Task 3: Server-side `SERVER_INFO_REQUEST` handler

**Files:**
- Modify: `server/core/src/session.cpp`

The handler replies with populated `ServerInfoReply` BEFORE the existing `CONNECT_REQUEST` gate. No session is created for the requesting peer. Peer usually disconnects after getting the reply.

- [ ] **Step 1: Locate packet dispatch**

```bash
grep -n "case PacketType::CONNECT_REQUEST" server/core/src/session.cpp
```

Note the switch's location. The new case goes INSIDE the same switch, BEFORE `CONNECT_REQUEST`.

- [ ] **Step 2: Add forward-decl for config access + session count**

If `session.cpp` doesn't already have access to the live `ServerConfig`, find where it's initialized (probably `core.cpp` stores it). Add a small accessor OR extern a static. The simplest path: `core.cpp` already passes config to `session` via some setter. If no such setter exists, add one:

In `server/core/src/session_api.h`, add near the top (under the existing forward-decls):

```cpp
struct ServerConfig;
void         session_bind_server_config(const ServerConfig* cfg);
```

In `session.cpp`, near the top (with other `static` state around line 50-70), add:

```cpp
#include "server_config.h"

static const ServerConfig* s_server_config = nullptr;

void session_bind_server_config(const ServerConfig* cfg) {
    s_server_config = cfg;
}
```

In `core.cpp`, in the server-start path (near where `session_bind_snapshot_store` is called), add:

```cpp
    kmp::session_bind_server_config(&cfg);  // `cfg` is the ServerConfig passed to worker_main
```

- [ ] **Step 3: Add the handler**

Find the `switch (header.type)` in `session.cpp` (around line 450 based on A.1). Add a new case BEFORE `CONNECT_REQUEST`:

```cpp
    case PacketType::SERVER_INFO_REQUEST: {
        ServerInfoRequest req;
        if (!unpack(data, length, req)) break;

        ServerInfoReply reply;
        reply.nonce             = req.nonce;
        reply.player_count      = static_cast<uint16_t>(s_sessions.size());
        reply.max_players       = s_server_config
            ? static_cast<uint16_t>(s_server_config->max_players)
            : 16;
        reply.protocol_version  = PROTOCOL_VERSION;
        reply.password_required = (s_server_config && !s_server_config->password.empty())
            ? 1 : 0;
        if (s_server_config) {
            std::strncpy(reply.description,
                         s_server_config->description.c_str(),
                         sizeof(reply.description) - 1);
            reply.description[sizeof(reply.description) - 1] = '\0';
        }

        auto buf = pack(reply);
        relay_send_to(peer, buf.data(), buf.size(), true);
        spdlog::debug("SERVER_INFO sent to {}: players={} version={}",
                      peer->address.host, reply.player_count,
                      static_cast<int>(reply.protocol_version));
        break;
    }
```

- [ ] **Step 4: Build**

```bash
make server-core 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add server/core/src/session.cpp server/core/src/session_api.h \
        server/core/src/core.cpp
git commit -m "feat(server): handle SERVER_INFO_REQUEST before CONNECT gate"
```

---

## Task 4: `server_list` data layer + tests

**Files:**
- Create: `core/src/server_list.h`
- Create: `core/src/server_list.cpp`
- Create: `tools/test_server_list.cpp`
- Modify: `core/CMakeLists.txt` (add source)
- Modify: `tools/CMakeLists.txt` (add test target)
- Modify: `Makefile` (`SNAPSHOT_UNIT_TESTS` += `test-server-list`)

A hand-rolled flat JSON reader/writer. VS2010 v100 STL can't link `nlohmann/json`. Our schema is flat (array of flat objects), so a minimal parser is ~100 lines.

- [ ] **Step 1: Write failing test**

Create `tools/test_server_list.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "server_list.h"

namespace fs = std::filesystem;
using namespace kmp;

static fs::path make_tempdir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("kmp_srvlist_" + std::string(name)
        + "_" + std::to_string(std::time(nullptr)));
    fs::create_directories(p);
    return p;
}

static void test_roundtrip() {
    fs::path dir = make_tempdir("roundtrip");
    std::string path = (dir / "servers.json").string();

    std::vector<ServerEntry> orig;
    ServerEntry e1;
    e1.id = "abcd1234"; e1.name = "Bob";
    e1.address = "1.2.3.4"; e1.port = 7777;
    e1.password = "hunter2"; e1.last_joined_ms = 1000;
    orig.push_back(e1);

    ServerEntry e2;
    e2.id = "ffff0000"; e2.name = "Alice's \"LAN\""; // quote in name
    e2.address = "192.168.1.5"; e2.port = 8888;
    e2.password = ""; e2.last_joined_ms = 0;
    orig.push_back(e2);

    KMP_CHECK(server_list_save_to(path, orig));

    std::vector<ServerEntry> loaded;
    KMP_CHECK(server_list_load_from(path, loaded));
    KMP_CHECK(loaded.size() == 2);
    KMP_CHECK(loaded[0].id == "abcd1234");
    KMP_CHECK(loaded[0].name == "Bob");
    KMP_CHECK(loaded[0].address == "1.2.3.4");
    KMP_CHECK(loaded[0].port == 7777);
    KMP_CHECK(loaded[0].password == "hunter2");
    KMP_CHECK(loaded[0].last_joined_ms == 1000);
    KMP_CHECK(loaded[1].id == "ffff0000");
    KMP_CHECK(loaded[1].name == "Alice's \"LAN\"");

    fs::remove_all(dir);
    printf("test_roundtrip OK\n");
}

static void test_missing_file_returns_false() {
    std::vector<ServerEntry> loaded;
    bool ok = server_list_load_from("C:/does/not/exist/kmp_srv.json", loaded);
    KMP_CHECK(!ok);
    KMP_CHECK(loaded.empty());
    printf("test_missing_file_returns_false OK\n");
}

static void test_corrupt_file_renamed() {
    fs::path dir = make_tempdir("corrupt");
    std::string path = (dir / "servers.json").string();
    std::ofstream f(path);
    f << "not json at all {{{";
    f.close();

    std::vector<ServerEntry> loaded;
    bool ok = server_list_load_from(path, loaded);
    KMP_CHECK(!ok);
    KMP_CHECK(loaded.empty());

    // Original file should be gone; a .corrupt-<ts> sibling should exist.
    KMP_CHECK(!fs::exists(path));
    bool found_corrupt = false;
    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().filename().string().find("servers.json.corrupt-") == 0) {
            found_corrupt = true; break;
        }
    }
    KMP_CHECK(found_corrupt);

    fs::remove_all(dir);
    printf("test_corrupt_file_renamed OK\n");
}

static void test_new_id_is_8_hex() {
    std::string id = server_list_new_id();
    KMP_CHECK(id.size() == 8);
    for (char c : id) {
        KMP_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    // Different on subsequent calls.
    std::string id2 = server_list_new_id();
    KMP_CHECK(id != id2);
    printf("test_new_id_is_8_hex OK\n");
}

int main() {
    test_roundtrip();
    test_missing_file_returns_false();
    test_corrupt_file_renamed();
    test_new_id_is_8_hex();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-server-list test_server_list.cpp
    ${CMAKE_SOURCE_DIR}/core/src/server_list.cpp)
target_include_directories(test-server-list PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(test-server-list PRIVATE kenshi-mp-common)
target_compile_features(test-server-list PRIVATE cxx_std_17)
```

Extend `Makefile`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip test-snapshot-uploader \
                       test-server-info-packets test-server-list
```

- [ ] **Step 2: Verify fails**

```bash
make test 2>&1 | tail -10
```

Expected: `server_list.h` not found.

- [ ] **Step 3: Create `core/src/server_list.h`**

```cpp
// server_list.h — Persistent list of multiplayer server entries.
//
// Stored as flat JSON at <Documents>/My Games/Kenshi/KenshiMP/servers.json.
// Hand-rolled parser — v100 toolchain can't link nlohmann/json.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kmp {

struct ServerEntry {
    std::string id;             // 8-hex-char, generated via server_list_new_id()
    std::string name;
    std::string address;
    uint16_t    port;
    std::string password;
    uint64_t    last_joined_ms;

    ServerEntry() : port(0), last_joined_ms(0) {}
};

/// Load entries from a specific path. Returns false if the file is missing
/// OR corrupt (in which case a corrupt file is renamed to
/// "<path>.corrupt-<unix_seconds>"). `out` is cleared on false.
bool server_list_load_from(const std::string& path, std::vector<ServerEntry>& out);

/// Save entries to a specific path. Writes to "<path>.tmp" then renames for
/// atomicity. Creates the parent directory if missing. Returns false on any
/// I/O failure.
bool server_list_save_to(const std::string& path, const std::vector<ServerEntry>& in);

/// Resolve the default path: <Documents>/My Games/Kenshi/KenshiMP/servers.json.
/// Returns empty string if SHGetFolderPath fails.
std::string server_list_default_path();

/// Load from the default path. Convenience wrapper around load_from +
/// default_path.
bool server_list_load(std::vector<ServerEntry>& out);

/// Save to the default path.
bool server_list_save(const std::vector<ServerEntry>& in);

/// Generate a random 8-hex-char identifier.
std::string server_list_new_id();

} // namespace kmp
```

- [ ] **Step 4: Create `core/src/server_list.cpp`**

```cpp
#include "server_list.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace kmp {

namespace {

// ---- hand-rolled flat JSON parser ----

struct Parser {
    const std::string& s;
    size_t i;
    Parser(const std::string& _s) : s(_s), i(0) {}
    void skip_ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool eof() { return i >= s.size(); }
    char peek() { return eof() ? '\0' : s[i]; }
    bool match(char c) { skip_ws(); if (eof() || s[i] != c) return false; ++i; return true; }
    bool parse_string(std::string& out) {
        skip_ws();
        if (!match('"')) return false;
        out.clear();
        while (!eof() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char esc = s[i + 1];
                if (esc == '"')       out += '"';
                else if (esc == '\\') out += '\\';
                else if (esc == 'n')  out += '\n';
                else if (esc == 't')  out += '\t';
                else if (esc == '/')  out += '/';
                else return false;
                i += 2;
            } else {
                out += s[i++];
            }
        }
        if (eof()) return false;
        ++i;  // closing "
        return true;
    }
    bool parse_uint64(uint64_t& out) {
        skip_ws();
        if (eof() || !(s[i] >= '0' && s[i] <= '9')) return false;
        out = 0;
        while (!eof() && s[i] >= '0' && s[i] <= '9') {
            out = out * 10 + static_cast<uint64_t>(s[i] - '0');
            ++i;
        }
        return true;
    }
};

static std::string escape_string(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static bool parse_entry(Parser& p, ServerEntry& e) {
    if (!p.match('{')) return false;
    bool first = true;
    while (true) {
        p.skip_ws();
        if (p.peek() == '}') { ++p.i; return true; }
        if (!first) { if (!p.match(',')) return false; }
        first = false;
        std::string key;
        if (!p.parse_string(key)) return false;
        if (!p.match(':')) return false;
        if (key == "id")                { if (!p.parse_string(e.id)) return false; }
        else if (key == "name")         { if (!p.parse_string(e.name)) return false; }
        else if (key == "address")      { if (!p.parse_string(e.address)) return false; }
        else if (key == "password")     { if (!p.parse_string(e.password)) return false; }
        else if (key == "port")         { uint64_t v; if (!p.parse_uint64(v)) return false; e.port = static_cast<uint16_t>(v); }
        else if (key == "last_joined_ms") { uint64_t v; if (!p.parse_uint64(v)) return false; e.last_joined_ms = v; }
        else { /* unknown key: skip — for now we only accept known keys, so fail */ return false; }
    }
}

static bool parse_document(const std::string& text, std::vector<ServerEntry>& out) {
    Parser p(text);
    if (!p.match('{')) return false;
    bool saw_version = false;
    bool saw_servers = false;
    while (true) {
        p.skip_ws();
        if (p.peek() == '}') { ++p.i; break; }
        if (saw_version || saw_servers) { if (!p.match(',')) return false; }
        std::string key;
        if (!p.parse_string(key)) return false;
        if (!p.match(':')) return false;
        if (key == "version") {
            uint64_t v; if (!p.parse_uint64(v)) return false;
            if (v != 1) return false;
            saw_version = true;
        } else if (key == "servers") {
            if (!p.match('[')) return false;
            bool first = true;
            while (true) {
                p.skip_ws();
                if (p.peek() == ']') { ++p.i; break; }
                if (!first) { if (!p.match(',')) return false; }
                first = false;
                ServerEntry e;
                if (!parse_entry(p, e)) return false;
                out.push_back(e);
            }
            saw_servers = true;
        } else {
            return false;
        }
    }
    return saw_version && saw_servers;
}

static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, NULL, NULL);
    return s;
}

} // namespace

bool server_list_load_from(const std::string& path, std::vector<ServerEntry>& out) {
    out.clear();
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    f.close();

    std::vector<ServerEntry> parsed;
    if (!parse_document(text, parsed)) {
        // Rename the corrupt file so we don't re-read it and don't clobber it.
        std::string dst = path + ".corrupt-" + std::to_string(static_cast<long long>(std::time(NULL)));
        std::error_code ec;
        fs::rename(path, dst, ec);
        return false;
    }
    out = parsed;
    return true;
}

bool server_list_save_to(const std::string& path, const std::vector<ServerEntry>& in) {
    fs::path p(path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    (void)ec;

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return false;
        f << "{\n  \"version\": 1,\n  \"servers\": [";
        for (size_t i = 0; i < in.size(); ++i) {
            const ServerEntry& e = in[i];
            f << (i > 0 ? ",\n" : "\n")
              << "    {\n"
              << "      \"id\": \""             << escape_string(e.id) << "\",\n"
              << "      \"name\": \""           << escape_string(e.name) << "\",\n"
              << "      \"address\": \""        << escape_string(e.address) << "\",\n"
              << "      \"port\": "             << static_cast<int>(e.port) << ",\n"
              << "      \"password\": \""       << escape_string(e.password) << "\",\n"
              << "      \"last_joined_ms\": "   << e.last_joined_ms << "\n"
              << "    }";
        }
        f << (in.empty() ? "]\n}\n" : "\n  ]\n}\n");
        if (!f.good()) return false;
    }

    std::error_code ec2;
    fs::remove(path, ec2);
    fs::rename(tmp, path, ec2);
    return !ec2;
}

std::string server_list_default_path() {
    wchar_t buf[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL,
                                    SHGFP_TYPE_CURRENT, buf))) {
        return std::string();
    }
    return wide_to_utf8(buf) + "\\My Games\\Kenshi\\KenshiMP\\servers.json";
}

bool server_list_load(std::vector<ServerEntry>& out) {
    std::string path = server_list_default_path();
    if (path.empty()) return false;
    return server_list_load_from(path, out);
}

bool server_list_save(const std::vector<ServerEntry>& in) {
    std::string path = server_list_default_path();
    if (path.empty()) return false;
    return server_list_save_to(path, in);
}

std::string server_list_new_id() {
    static bool seeded = false;
    if (!seeded) { std::srand(static_cast<unsigned>(std::time(NULL)) ^ GetCurrentProcessId()); seeded = true; }
    char buf[9];
    for (int i = 0; i < 8; ++i) {
        int v = std::rand() & 0xF;
        buf[i] = (v < 10) ? ('0' + v) : ('a' + v - 10);
    }
    buf[8] = '\0';
    return std::string(buf);
}

} // namespace kmp
```

- [ ] **Step 5: Register in `core/CMakeLists.txt`**

Add `src/server_list.cpp` to the `add_library(KenshiMP SHARED ...)` source list.

- [ ] **Step 6: Build + run**

```bash
make core 2>&1 | tail -5
make test 2>&1 | tail -15
```

Expected:
```
--- test-server-list ---
test_roundtrip OK
test_missing_file_returns_false OK
test_corrupt_file_renamed OK
test_new_id_is_8_hex OK
ALL PASS
```

- [ ] **Step 7: Commit**

```bash
git add core/src/server_list.h core/src/server_list.cpp core/CMakeLists.txt \
        tools/test_server_list.cpp tools/CMakeLists.txt Makefile
git commit -m "feat(core): server_list JSON persistence with corrupt-file recovery"
```

---

## Task 5: `server_pinger` state machine + tests

**Files:**
- Create: `core/src/server_pinger.h`
- Create: `core/src/server_pinger.cpp`
- Create: `tools/test_server_pinger.cpp`
- Modify: `core/CMakeLists.txt`
- Modify: `tools/CMakeLists.txt`
- Modify: `Makefile`

DI-style state machine, one batch per browser open. Injected hooks for ENet send/recv/connect + clock. Tests never touch real ENet.

### API design

One `ServerPinger` owns a dictionary of in-progress pings keyed by `entry.id`. Deps provide:
- `connect(id, address, port)` — kick off an ENet connect, non-blocking. Return false on DNS resolve failure.
- `send_request(id, nonce)` — call after the connect event fires; non-blocking.
- `disconnect(id)` — tear down the ENet connection (after reply received or timeout).
- `now_seconds()` — monotonic clock.
- `on_result(id, result)` — callback when each ping finalises.

Events fed in from outside (driven by whoever polls ENet in the real impl):
- `pinger.on_connected(id)` — ENet CONNECT fired.
- `pinger.on_reply(id, nonce, fields)` — a SERVER_INFO_REPLY received.
- `pinger.on_disconnect(id)` — ENet disconnect (peer dropped).

### Step 1: Write failing test

Create `tools/test_server_pinger.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "server_pinger.h"

using namespace kmp;

namespace {

struct MockEnet {
    std::map<std::string, bool> connect_ok;     // id -> returned true
    std::vector<std::pair<std::string, uint32_t>> sent_requests;
    std::vector<std::string> disconnects;
};

static ServerPinger::Deps make_deps(MockEnet* mock, float* clock) {
    ServerPinger::Deps d;
    d.connect = [mock](const std::string& id,
                       const std::string& addr, uint16_t port) -> bool {
        (void)addr; (void)port;
        auto it = mock->connect_ok.find(id);
        bool ok = (it == mock->connect_ok.end()) ? true : it->second;
        return ok;
    };
    d.send_request = [mock](const std::string& id, uint32_t nonce) {
        mock->sent_requests.emplace_back(id, nonce);
    };
    d.disconnect = [mock](const std::string& id) {
        mock->disconnects.push_back(id);
    };
    d.now_seconds = [clock]() { return *clock; };
    return d;
}

static void test_happy_path() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("s1", "1.2.3.4", 7777);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::Connecting);

    pinger.on_connected("s1");
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::AwaitingReply);
    KMP_CHECK(mock.sent_requests.size() == 1);
    KMP_CHECK(mock.sent_requests[0].first == "s1");
    uint32_t nonce = mock.sent_requests[0].second;

    ServerPinger::ReplyFields f;
    f.player_count = 3; f.max_players = 16;
    f.protocol_version = PROTOCOL_VERSION;
    f.password_required = 0;
    std::strncpy(f.description, "Test server", sizeof(f.description));
    clock = 0.042f;
    pinger.on_reply("s1", nonce, f);

    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::Success);
    const ServerPinger::Result& r = pinger.result("s1");
    KMP_CHECK(r.ping_ms > 0 && r.ping_ms < 100);  // 42ms after start
    KMP_CHECK(r.player_count == 3);
    KMP_CHECK(r.max_players == 16);
    KMP_CHECK(!mock.disconnects.empty() && mock.disconnects.back() == "s1");

    printf("test_happy_path OK\n");
}

static void test_connect_fail_marks_dns_error() {
    MockEnet mock;
    mock.connect_ok["bad"] = false;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("bad", "not.a.host", 7777);
    KMP_CHECK(pinger.status("bad") == ServerPinger::Status::DnsError);

    printf("test_connect_fail_marks_dns_error OK\n");
}

static void test_connect_timeout() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("slow", "1.2.3.4", 7777);
    // Advance 2.1s without on_connected.
    clock = 2.1f;
    pinger.tick();
    KMP_CHECK(pinger.status("slow") == ServerPinger::Status::Offline);

    printf("test_connect_timeout OK\n");
}

static void test_reply_timeout() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("mute", "1.2.3.4", 7777);
    pinger.on_connected("mute");
    KMP_CHECK(pinger.status("mute") == ServerPinger::Status::AwaitingReply);
    clock = 2.1f;
    pinger.tick();
    KMP_CHECK(pinger.status("mute") == ServerPinger::Status::NoReply);

    printf("test_reply_timeout OK\n");
}

static void test_nonce_mismatch_ignored() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("s1", "1.2.3.4", 7777);
    pinger.on_connected("s1");
    uint32_t real_nonce = mock.sent_requests[0].second;
    uint32_t bad_nonce = real_nonce ^ 0xFFFFFFFF;

    ServerPinger::ReplyFields f; f.player_count = 9;
    pinger.on_reply("s1", bad_nonce, f);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::AwaitingReply);

    pinger.on_reply("s1", real_nonce, f);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::Success);

    printf("test_nonce_mismatch_ignored OK\n");
}

static void test_version_mismatch_recorded() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("s1", "1.2.3.4", 7777);
    pinger.on_connected("s1");
    uint32_t nonce = mock.sent_requests[0].second;

    ServerPinger::ReplyFields f;
    f.protocol_version = static_cast<uint8_t>(PROTOCOL_VERSION + 1);
    f.player_count = 0; f.max_players = 0; f.password_required = 0;
    pinger.on_reply("s1", nonce, f);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::VersionMismatch);
    KMP_CHECK(pinger.result("s1").protocol_version == PROTOCOL_VERSION + 1);

    printf("test_version_mismatch_recorded OK\n");
}

} // namespace

int main() {
    test_happy_path();
    test_connect_fail_marks_dns_error();
    test_connect_timeout();
    test_reply_timeout();
    test_nonce_mismatch_ignored();
    test_version_mismatch_recorded();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-server-pinger test_server_pinger.cpp
    ${CMAKE_SOURCE_DIR}/core/src/server_pinger.cpp)
target_include_directories(test-server-pinger PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(test-server-pinger PRIVATE kenshi-mp-common)
target_compile_features(test-server-pinger PRIVATE cxx_std_17)
```

Extend `Makefile`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip test-snapshot-uploader \
                       test-server-info-packets test-server-list \
                       test-server-pinger
```

- [ ] **Step 2: Verify fails**

```bash
make test 2>&1 | tail -10
```

Expected: `server_pinger.h` not found.

- [ ] **Step 3: Create `core/src/server_pinger.h`**

```cpp
// server_pinger.h — Batch ENet pinger that resolves SERVER_INFO_REPLY
// per entry. DI interface keeps the real ENet host out of the state
// machine so tests run synchronously without sockets.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>

#include "protocol.h"

namespace kmp {

class ServerPinger {
public:
    struct Status { enum E {
        Idle,
        Connecting,        // started, no ENet connect event yet
        AwaitingReply,     // connected, sent request, waiting for reply
        Success,
        DnsError,          // connect() returned false
        Offline,           // connect timeout (no CONNECT event within 2s)
        NoReply,           // reply timeout (no REPLY within 2s of CONNECT)
        VersionMismatch,   // reply arrived but protocol_version != ours
    }; };

    struct ReplyFields {
        uint16_t player_count;
        uint16_t max_players;
        uint8_t  protocol_version;
        uint8_t  password_required;
        char     description[128];
        ReplyFields() : player_count(0), max_players(0),
                        protocol_version(0), password_required(0) {
            std::memset(description, 0, sizeof(description));
        }
    };

    struct Result {
        Status::E  status;
        uint32_t   ping_ms;          // only valid on Success
        uint16_t   player_count;
        uint16_t   max_players;
        uint8_t    protocol_version;
        uint8_t    password_required;
        char       description[128];
        Result() : status(Status::Idle), ping_ms(0), player_count(0),
                   max_players(0), protocol_version(0), password_required(0) {
            std::memset(description, 0, sizeof(description));
        }
    };

    struct Deps {
        std::function<bool(const std::string& id, const std::string& address, uint16_t port)> connect;
        std::function<void(const std::string& id, uint32_t nonce)> send_request;
        std::function<void(const std::string& id)> disconnect;
        std::function<float()> now_seconds;
    };

    explicit ServerPinger(Deps deps);

    /// Begin a ping. Sets status = Connecting (or DnsError if connect fails).
    /// Replaces any pre-existing state for the same id.
    void start(const std::string& id, const std::string& address, uint16_t port);

    /// Drive timeouts. Should be called each frame while any ping is in flight.
    void tick();

    /// ENet CONNECT event for this id. Sends request, transitions to AwaitingReply.
    void on_connected(const std::string& id);

    /// SERVER_INFO_REPLY received for this id.
    void on_reply(const std::string& id, uint32_t nonce, const ReplyFields& f);

    /// Accessors.
    Status::E         status(const std::string& id) const;
    const Result&     result(const std::string& id) const;

    /// Clear all pings. Disconnect is called for any in-flight.
    void clear();

private:
    struct Slot {
        Status::E status;
        float     start_t;         // wall time set on start()
        float     connected_t;     // wall time set on on_connected()
        uint32_t  nonce;
        Result    result;
    };
    Deps m_deps;
    std::map<std::string, Slot> m_slots;
    static const Result kEmpty;

    static uint32_t random_nonce();
};

} // namespace kmp
```

- [ ] **Step 4: Create `core/src/server_pinger.cpp`**

```cpp
#include "server_pinger.h"

#include <cstdlib>

namespace kmp {

const ServerPinger::Result ServerPinger::kEmpty;

static const float kConnectTimeoutSec = 2.0f;
static const float kReplyTimeoutSec   = 2.0f;

uint32_t ServerPinger::random_nonce() {
    return (static_cast<uint32_t>(std::rand()) << 16)
         ^ static_cast<uint32_t>(std::rand());
}

ServerPinger::ServerPinger(Deps deps) : m_deps(deps) {}

void ServerPinger::start(const std::string& id,
                         const std::string& address, uint16_t port) {
    Slot& s = m_slots[id];
    s.status      = Status::Connecting;
    s.start_t     = m_deps.now_seconds();
    s.connected_t = 0.0f;
    s.nonce       = random_nonce();
    s.result      = Result();

    bool ok = m_deps.connect(id, address, port);
    if (!ok) {
        s.status = Status::DnsError;
        s.result.status = Status::DnsError;
    }
}

void ServerPinger::tick() {
    float now = m_deps.now_seconds();
    for (auto& kv : m_slots) {
        Slot& s = kv.second;
        if (s.status == Status::Connecting) {
            if (now - s.start_t > kConnectTimeoutSec) {
                s.status = Status::Offline;
                s.result.status = Status::Offline;
                m_deps.disconnect(kv.first);
            }
        } else if (s.status == Status::AwaitingReply) {
            if (now - s.connected_t > kReplyTimeoutSec) {
                s.status = Status::NoReply;
                s.result.status = Status::NoReply;
                m_deps.disconnect(kv.first);
            }
        }
    }
}

void ServerPinger::on_connected(const std::string& id) {
    auto it = m_slots.find(id);
    if (it == m_slots.end()) return;
    Slot& s = it->second;
    if (s.status != Status::Connecting) return;
    s.status      = Status::AwaitingReply;
    s.connected_t = m_deps.now_seconds();
    m_deps.send_request(id, s.nonce);
}

void ServerPinger::on_reply(const std::string& id, uint32_t nonce,
                            const ReplyFields& f) {
    auto it = m_slots.find(id);
    if (it == m_slots.end()) return;
    Slot& s = it->second;
    if (s.status != Status::AwaitingReply) return;
    if (nonce != s.nonce) return;

    s.result.player_count      = f.player_count;
    s.result.max_players       = f.max_players;
    s.result.protocol_version  = f.protocol_version;
    s.result.password_required = f.password_required;
    std::memcpy(s.result.description, f.description, sizeof(s.result.description));

    if (f.protocol_version != PROTOCOL_VERSION) {
        s.status = Status::VersionMismatch;
        s.result.status = Status::VersionMismatch;
    } else {
        float now = m_deps.now_seconds();
        s.result.ping_ms = static_cast<uint32_t>((now - s.start_t) * 1000.0f);
        s.status = Status::Success;
        s.result.status = Status::Success;
    }
    m_deps.disconnect(id);
}

ServerPinger::Status::E ServerPinger::status(const std::string& id) const {
    auto it = m_slots.find(id);
    if (it == m_slots.end()) return Status::Idle;
    return it->second.status;
}

const ServerPinger::Result& ServerPinger::result(const std::string& id) const {
    auto it = m_slots.find(id);
    if (it == m_slots.end()) return kEmpty;
    return it->second.result;
}

void ServerPinger::clear() {
    for (auto& kv : m_slots) {
        if (kv.second.status == Status::Connecting ||
            kv.second.status == Status::AwaitingReply) {
            m_deps.disconnect(kv.first);
        }
    }
    m_slots.clear();
}

} // namespace kmp
```

- [ ] **Step 5: Register in core CMakeLists + build + test**

Add `src/server_pinger.cpp` to the `add_library(KenshiMP SHARED ...)` source list.

```bash
make core 2>&1 | tail -5
make test 2>&1 | tail -15
```

Expected:
```
--- test-server-pinger ---
test_happy_path OK
test_connect_fail_marks_dns_error OK
test_connect_timeout OK
test_reply_timeout OK
test_nonce_mismatch_ignored OK
test_version_mismatch_recorded OK
ALL PASS
```

- [ ] **Step 6: Commit**

```bash
git add core/src/server_pinger.h core/src/server_pinger.cpp \
        core/CMakeLists.txt \
        tools/test_server_pinger.cpp tools/CMakeLists.txt Makefile
git commit -m "feat(core): ServerPinger state machine with DI tests"
```

---

## Task 6: End-to-end integration test for SERVER_INFO

**Files:**
- Create: `tools/test_server_info_e2e.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `Makefile` (extend the e2e test block)

Verifies that a live headless server responds to `SERVER_INFO_REQUEST` with the right metadata. Parallels Plan A.1's `test-snapshot-e2e`.

- [ ] **Step 1: Create `tools/test_server_info_e2e.cpp`**

```cpp
// test_server_info_e2e.cpp — Live ping: connect to a headless server,
// send SERVER_INFO_REQUEST, verify a valid REPLY comes back.
//
// Requires: ./build_server/bin/Release/kenshi-mp-server-headless.exe
// running on 127.0.0.1:7777.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <enet/enet.h>

#include "packets.h"
#include "serialization.h"

using namespace kmp;

static int fail(const char* msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

int main(int argc, char** argv) {
    const char* host_ip   = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t    enet_port = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 7777;

    if (enet_initialize() != 0) return fail("enet_initialize");

    ENetHost* client = enet_host_create(NULL, 1, 2, 0, 0);
    if (!client) return fail("enet_host_create");

    ENetAddress addr;
    if (enet_address_set_host(&addr, host_ip) != 0) return fail("enet_address_set_host");
    addr.port = enet_port;

    ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
    if (!peer) return fail("enet_host_connect");

    ENetEvent ev;
    bool connected = false;
    for (int i = 0; i < 50 && !connected; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
    }
    if (!connected) return fail("ENet handshake timeout");

    ServerInfoRequest req; req.nonce = 0x12345678;
    auto buf = pack(req);
    ENetPacket* pkt = enet_packet_create(buf.data(), buf.size(),
                                         ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pkt);
    enet_host_flush(client);

    bool got_reply = false;
    for (int i = 0; i < 50 && !got_reply; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_RECEIVE) {
            PacketHeader h;
            if (peek_header(ev.packet->data, ev.packet->dataLength, h) &&
                h.type == PacketType::SERVER_INFO_REPLY) {
                ServerInfoReply reply;
                if (unpack(ev.packet->data, ev.packet->dataLength, reply)) {
                    if (reply.nonce != 0x12345678) {
                        fprintf(stderr, "nonce mismatch: got 0x%x\n", reply.nonce);
                        enet_packet_destroy(ev.packet);
                        return 1;
                    }
                    printf("SERVER_INFO_REPLY: players=%u/%u version=%u "
                           "password_required=%u desc=\"%s\"\n",
                           reply.player_count, reply.max_players,
                           reply.protocol_version, reply.password_required,
                           reply.description);
                    got_reply = true;
                }
            }
            enet_packet_destroy(ev.packet);
        }
    }
    if (!got_reply) return fail("no SERVER_INFO_REPLY received");

    enet_peer_disconnect(peer, 0);
    enet_host_flush(client);
    enet_host_destroy(client);
    enet_deinitialize();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-server-info-e2e test_server_info_e2e.cpp)
target_include_directories(test-server-info-e2e PRIVATE
    ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(test-server-info-e2e PRIVATE kenshi-mp-common ws2_32 winmm)
target_compile_features(test-server-info-e2e PRIVATE cxx_std_17)
if(ENET_DIR)
    target_include_directories(test-server-info-e2e PRIVATE ${ENET_DIR}/include)
    target_link_directories(test-server-info-e2e PRIVATE
        ${ENET_DIR}/lib ${ENET_DIR}/build/Release ${ENET_DIR}/build/Debug)
    target_link_libraries(test-server-info-e2e PRIVATE enet)
endif()
```

Extend `Makefile`:

```makefile
SNAPSHOT_E2E_TESTS  := test-snapshot-e2e test-server-info-e2e
```

(And update the `test-e2e` make target to run each e2e one by one, similar to Task A.1. If it's already a for-loop over `SNAPSHOT_E2E_TESTS`, no change needed.)

- [ ] **Step 2: Build**

```bash
make test 2>&1 | tail -5
```

The e2e target builds but doesn't run automatically (see `make test-e2e`).

- [ ] **Step 3: Run against live server**

```bash
./build_server/bin/Release/kenshi-mp-server-headless.exe > /tmp/ping-srv.log 2>&1 &
SPID=$!
sleep 2
./build_server/tools/Release/test-server-info-e2e.exe
EC=$?
kill $SPID 2>/dev/null
taskkill //F //PID $SPID 2>/dev/null || true
echo "exit=$EC"
```

Expected output:
```
SERVER_INFO_REPLY: players=0/16 version=1 password_required=0 desc=""
ALL PASS
exit=0
```

Edit `server/config.json` (or wherever the headless reads from) to set `"description": "Plan A.3 test"` and rerun — expect the description in the output.

- [ ] **Step 4: Commit**

```bash
git add tools/test_server_info_e2e.cpp tools/CMakeLists.txt Makefile
git commit -m "test(server): SERVER_INFO_REQUEST end-to-end"
```

---

## Task 7: Server browser MyGUI window

**Files:**
- Create: `core/src/server_browser.h`
- Create: `core/src/server_browser.cpp`
- Modify: `core/CMakeLists.txt` (add source)

This is the biggest task. No unit tests — MyGUI widgets aren't easily testable in isolation. The unit tests for `server_list` and `server_pinger` cover the non-UI logic; browser is manually verified in Task 8.

### API

```cpp
void server_browser_init();     // call once from plugin startup
void server_browser_shutdown(); // call once (no-op if never opened)
void server_browser_open();     // show the modal, load servers.json, begin pinging
void server_browser_close();    // hide, stop pings
void server_browser_tick(float dt); // called every frame from player_sync_tick
bool server_browser_is_open();
```

### Step 1: Create `core/src/server_browser.h`

```cpp
// server_browser.h — MyGUI modal listing saved KenshiMP servers with
// live ping + player count + description. See spec
// docs/superpowers/specs/2026-04-22-server-browser-ui-design.md
#pragma once

namespace kmp {

void server_browser_init();
void server_browser_shutdown();
void server_browser_open();
void server_browser_close();
void server_browser_tick(float dt);
bool server_browser_is_open();

} // namespace kmp
```

### Step 2: Create `core/src/server_browser.cpp`

This is large (~450 lines). Build it section by section. The full file:

```cpp
// server_browser.cpp — See server_browser.h for overview.
#include "server_browser.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MyGUI.h>
#include <OgreLogManager.h>

#include <enet/enet.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kmp_log.h"
#include "packets.h"
#include "serialization.h"
#include "server_list.h"
#include "server_pinger.h"

namespace kmp {

namespace {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct RowWidgets {
    MyGUI::Widget*  root;
    MyGUI::TextBox* line1;
    MyGUI::TextBox* line2;
};

static bool                          s_open = false;
static MyGUI::Window*                s_window = NULL;
static MyGUI::Button*                s_btn_refresh = NULL;
static MyGUI::Button*                s_btn_direct  = NULL;
static MyGUI::Button*                s_btn_add     = NULL;
static MyGUI::Button*                s_btn_edit    = NULL;
static MyGUI::Button*                s_btn_remove  = NULL;
static MyGUI::Button*                s_btn_join    = NULL;
static MyGUI::ScrollView*            s_list_scroll = NULL;
static std::vector<ServerEntry>      s_entries;
static std::map<std::string, RowWidgets> s_rows;   // id -> widgets
static std::string                   s_selected_id;

// Ping transport.
static ENetHost*                     s_ping_host = NULL;
static std::unique_ptr<ServerPinger> s_pinger;
static std::map<ENetPeer*, std::string> s_peer_to_id;
static std::map<std::string, ENetPeer*> s_id_to_peer;

// Sub-dialogs (created lazily, hidden when not in use).
static MyGUI::Window*   s_add_window = NULL;
static MyGUI::EditBox*  s_add_name   = NULL;
static MyGUI::EditBox*  s_add_addr   = NULL;
static MyGUI::EditBox*  s_add_port   = NULL;
static MyGUI::EditBox*  s_add_pw     = NULL;
static MyGUI::TextBox*  s_add_err    = NULL;
static bool             s_add_is_edit = false;  // true = editing selected row
static MyGUI::Button*   s_add_ok     = NULL;
static MyGUI::Button*   s_add_cancel = NULL;

static MyGUI::Window*   s_confirm_window = NULL;
static MyGUI::TextBox*  s_confirm_label  = NULL;

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
static void rebuild_rows();
static void update_button_states();
static void start_all_pings();
static void stop_all_pings();
static void apply_ping_to_row(const std::string& id);

// ---------------------------------------------------------------------------
// MyGUI callbacks
// ---------------------------------------------------------------------------
static void on_window_button(MyGUI::Window* /*sender*/, const std::string& name) {
    if (name == "close") server_browser_close();
}

static void on_refresh(MyGUI::Widget*) { start_all_pings(); }

static void on_row_click(MyGUI::Widget* sender) {
    std::string id = sender->getUserString("kmp_row_id");
    if (id.empty()) return;
    s_selected_id = id;
    rebuild_rows();
    update_button_states();
}

static void on_add(MyGUI::Widget*);
static void on_edit(MyGUI::Widget*);
static void on_remove(MyGUI::Widget*);
static void on_direct(MyGUI::Widget*);
static void on_join(MyGUI::Widget*);
static void on_add_ok(MyGUI::Widget*);
static void on_add_cancel(MyGUI::Widget*);

// ---------------------------------------------------------------------------
// Init / Shutdown / Open / Close
// ---------------------------------------------------------------------------
static void create_main_window() {
    if (s_window) return;
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;

    s_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_WindowCX",
        MyGUI::IntCoord(0, 0, 600, 460),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_BrowserWindow"
    );
    s_window->setCaption("Multiplayer Servers");
    s_window->setVisible(false);
    s_window->eventWindowButtonPressed += MyGUI::newDelegate(on_window_button);

    const int pad = 6;
    s_btn_refresh = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(pad, pad, 90, 28),
        MyGUI::Align::Left | MyGUI::Align::Top);
    s_btn_refresh->setCaption("Refresh");
    s_btn_refresh->eventMouseButtonClick += MyGUI::newDelegate(on_refresh);

    s_list_scroll = s_window->createWidget<MyGUI::ScrollView>(
        "Kenshi_ScrollViewSkin",
        MyGUI::IntCoord(pad, pad + 32,
                        s_window->getClientCoord().width  - 2 * pad,
                        s_window->getClientCoord().height - 32 - 40 - 3 * pad),
        MyGUI::Align::Stretch);
    s_list_scroll->setVisibleHScroll(false);
    s_list_scroll->setVisibleVScroll(true);

    int by = s_window->getClientCoord().height - 40 - pad;
    int bx = pad;
    s_btn_direct = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 110, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_direct->setCaption("Direct Connect");
    s_btn_direct->eventMouseButtonClick += MyGUI::newDelegate(on_direct);
    bx += 116;
    s_btn_add = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 70, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_add->setCaption("Add");
    s_btn_add->eventMouseButtonClick += MyGUI::newDelegate(on_add);
    bx += 76;
    s_btn_edit = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 70, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_edit->setCaption("Edit");
    s_btn_edit->eventMouseButtonClick += MyGUI::newDelegate(on_edit);
    bx += 76;
    s_btn_remove = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 80, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_remove->setCaption("Remove");
    s_btn_remove->eventMouseButtonClick += MyGUI::newDelegate(on_remove);
    bx += 86;
    s_btn_join = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 80, 32), MyGUI::Align::Right | MyGUI::Align::Bottom);
    s_btn_join->setCaption("Join");
    s_btn_join->eventMouseButtonClick += MyGUI::newDelegate(on_join);
}

static void create_add_window() {
    if (s_add_window) return;
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;
    s_add_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_WindowCX",
        MyGUI::IntCoord(0, 0, 320, 260),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_BrowserAddWindow");
    s_add_window->setCaption("Add Server");
    s_add_window->setVisible(false);
    s_add_window->eventWindowButtonPressed += MyGUI::newDelegate(
        [](MyGUI::Window*, const std::string& name) {
            if (name == "close") s_add_window->setVisible(false);
        });

    const int pad = 8, lx = pad, lw = 80, fx = pad + 90, fw = 210, rowh = 28;
    int y = pad;
    auto mk_label = [&](const char* cap) {
        MyGUI::TextBox* t = s_add_window->createWidget<MyGUI::TextBox>(
            "Kenshi_TextBoxEmptySkin",
            MyGUI::IntCoord(lx, y, lw, rowh), MyGUI::Align::Default);
        t->setCaption(cap);
        t->setFontName("Kenshi_StandardFont_Medium");
    };
    auto mk_field = [&]() -> MyGUI::EditBox* {
        return s_add_window->createWidget<MyGUI::EditBox>(
            "Kenshi_EditBox",
            MyGUI::IntCoord(fx, y, fw, rowh), MyGUI::Align::Default);
    };
    mk_label("Name:");    s_add_name = mk_field(); y += rowh + 4;
    mk_label("Address:"); s_add_addr = mk_field(); y += rowh + 4;
    mk_label("Port:");    s_add_port = mk_field(); y += rowh + 4;
    mk_label("Password:");s_add_pw   = mk_field(); y += rowh + 8;

    s_add_err = s_add_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(pad, y, 300, rowh), MyGUI::Align::Default);
    s_add_err->setTextColour(MyGUI::Colour(1.0f, 0.3f, 0.3f));
    s_add_err->setCaption("");
    y += rowh + 4;

    s_add_ok = s_add_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(pad, y, 90, 30), MyGUI::Align::Default);
    s_add_ok->setCaption("OK");
    s_add_ok->eventMouseButtonClick += MyGUI::newDelegate(on_add_ok);
    s_add_cancel = s_add_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(pad + 100, y, 90, 30), MyGUI::Align::Default);
    s_add_cancel->setCaption("Cancel");
    s_add_cancel->eventMouseButtonClick += MyGUI::newDelegate(on_add_cancel);
}

void server_browser_init() {
    // Deferred: window is created lazily on first open so MyGUI is ready.
}

void server_browser_shutdown() {
    stop_all_pings();
    if (s_pinger) s_pinger.reset();
}

void server_browser_open() {
    if (s_open) return;
    create_main_window();
    if (!s_window) return;
    create_add_window();

    s_entries.clear();
    server_list_load(s_entries);

    s_selected_id.clear();
    s_open = true;
    s_window->setVisible(true);
    MyGUI::LayerManager::getInstance().upLayerItem(s_window);
    rebuild_rows();
    update_button_states();
    start_all_pings();
}

void server_browser_close() {
    if (!s_open) return;
    s_open = false;
    stop_all_pings();
    if (s_window) s_window->setVisible(false);
    if (s_add_window) s_add_window->setVisible(false);
    if (s_confirm_window) s_confirm_window->setVisible(false);
}

bool server_browser_is_open() { return s_open; }

// ---------------------------------------------------------------------------
// Ping
// ---------------------------------------------------------------------------
static float clock_seconds() {
    static LARGE_INTEGER freq, t0;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        init = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(now.QuadPart - t0.QuadPart) /
           static_cast<float>(freq.QuadPart);
}

static void start_all_pings() {
    stop_all_pings();
    if (s_entries.empty()) return;

    s_ping_host = enet_host_create(NULL,
        static_cast<size_t>(s_entries.size() + 1), 2, 0, 0);
    if (!s_ping_host) {
        KMP_LOG("[KenshiMP] browser: enet_host_create failed");
        return;
    }

    ServerPinger::Deps d;
    d.connect = [](const std::string& id,
                   const std::string& address, uint16_t port) -> bool {
        ENetAddress a;
        if (enet_address_set_host(&a, address.c_str()) != 0) return false;
        a.port = port;
        ENetPeer* peer = enet_host_connect(s_ping_host, &a, 2, 0);
        if (!peer) return false;
        s_peer_to_id[peer] = id;
        s_id_to_peer[id]   = peer;
        return true;
    };
    d.send_request = [](const std::string& id, uint32_t nonce) {
        auto it = s_id_to_peer.find(id);
        if (it == s_id_to_peer.end() || !it->second) return;
        ServerInfoRequest req; req.nonce = nonce;
        auto buf = pack(req);
        ENetPacket* pkt = enet_packet_create(buf.data(), buf.size(),
                                             ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(it->second, 0, pkt);
        enet_host_flush(s_ping_host);
    };
    d.disconnect = [](const std::string& id) {
        auto it = s_id_to_peer.find(id);
        if (it == s_id_to_peer.end() || !it->second) return;
        enet_peer_disconnect_later(it->second, 0);
    };
    d.now_seconds = []() { return clock_seconds(); };
    s_pinger.reset(new ServerPinger(d));

    for (const ServerEntry& e : s_entries) {
        s_pinger->start(e.id, e.address, e.port);
        apply_ping_to_row(e.id);
    }
}

static void stop_all_pings() {
    if (s_pinger) s_pinger->clear();
    if (s_ping_host) {
        enet_host_flush(s_ping_host);
        enet_host_destroy(s_ping_host);
        s_ping_host = NULL;
    }
    s_peer_to_id.clear();
    s_id_to_peer.clear();
}

static void poll_ping_events() {
    if (!s_ping_host || !s_pinger) return;
    ENetEvent ev;
    while (enet_host_service(s_ping_host, &ev, 0) > 0) {
        auto it = s_peer_to_id.find(ev.peer);
        std::string id = (it == s_peer_to_id.end()) ? std::string() : it->second;
        if (ev.type == ENET_EVENT_TYPE_CONNECT && !id.empty()) {
            s_pinger->on_connected(id);
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE && !id.empty()) {
            PacketHeader h;
            if (peek_header(ev.packet->data, ev.packet->dataLength, h) &&
                h.type == PacketType::SERVER_INFO_REPLY) {
                ServerInfoReply reply;
                if (unpack(ev.packet->data, ev.packet->dataLength, reply)) {
                    ServerPinger::ReplyFields f;
                    f.player_count      = reply.player_count;
                    f.max_players       = reply.max_players;
                    f.protocol_version  = reply.protocol_version;
                    f.password_required = reply.password_required;
                    std::memcpy(f.description, reply.description, sizeof(f.description));
                    s_pinger->on_reply(id, reply.nonce, f);
                    apply_ping_to_row(id);
                    update_button_states();
                }
            }
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT && !id.empty()) {
            s_id_to_peer.erase(id);
            s_peer_to_id.erase(ev.peer);
        }
    }
    s_pinger->tick();
    for (const ServerEntry& e : s_entries) apply_ping_to_row(e.id);
}

void server_browser_tick(float /*dt*/) {
    if (!s_open) return;
    poll_ping_events();
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------
static std::string format_ping_line(const ServerEntry& e) {
    if (!s_pinger) return "…";
    ServerPinger::Status::E st = s_pinger->status(e.id);
    const ServerPinger::Result& r = s_pinger->result(e.id);
    char buf[128];
    switch (st) {
    case ServerPinger::Status::Success:
        std::snprintf(buf, sizeof(buf), "%u/%u   %ums   %s",
            r.player_count, r.max_players, r.ping_ms,
            r.password_required ? "LOCK" : "");
        return buf;
    case ServerPinger::Status::Connecting:     return "connecting…";
    case ServerPinger::Status::AwaitingReply:  return "waiting…";
    case ServerPinger::Status::DnsError:       return "— DNS error";
    case ServerPinger::Status::Offline:        return "— offline";
    case ServerPinger::Status::NoReply:        return "— no reply";
    case ServerPinger::Status::VersionMismatch:return "— version mismatch";
    default: return "…";
    }
}

static void rebuild_rows() {
    if (!s_list_scroll) return;
    for (auto& kv : s_rows) {
        if (kv.second.root) s_list_scroll->destroyWidget(kv.second.root);
    }
    s_rows.clear();

    const int row_h = 44;
    int y = 0;
    for (const ServerEntry& e : s_entries) {
        RowWidgets rw;
        rw.root = s_list_scroll->createWidget<MyGUI::Button>(
            "Kenshi_Button1Skin",
            MyGUI::IntCoord(0, y, s_list_scroll->getViewCoord().width, row_h),
            MyGUI::Align::HStretch | MyGUI::Align::Top);
        rw.root->setUserString("kmp_row_id", e.id);
        rw.root->setCaption("");
        rw.root->eventMouseButtonClick += MyGUI::newDelegate(on_row_click);
        if (e.id == s_selected_id) rw.root->setStateSelected(true);

        rw.line1 = rw.root->createWidget<MyGUI::TextBox>(
            "Kenshi_TextBoxEmptySkin",
            MyGUI::IntCoord(4, 2, 580, 20), MyGUI::Align::Default);
        rw.line1->setFontName("Kenshi_StandardFont_Medium");
        rw.line1->setCaption(e.name + "   —   " + format_ping_line(e));

        rw.line2 = rw.root->createWidget<MyGUI::TextBox>(
            "Kenshi_TextBoxEmptySkin",
            MyGUI::IntCoord(4, 22, 580, 18), MyGUI::Align::Default);
        rw.line2->setFontName("Kenshi_StandardFont_Medium");
        char subbuf[256];
        std::snprintf(subbuf, sizeof(subbuf), "%s:%u",
            e.address.c_str(), static_cast<unsigned>(e.port));
        std::string sub = subbuf;
        if (s_pinger && s_pinger->status(e.id) == ServerPinger::Status::Success &&
            s_pinger->result(e.id).description[0]) {
            sub += "   \"";
            sub += s_pinger->result(e.id).description;
            sub += "\"";
        }
        rw.line2->setCaption(sub);

        s_rows[e.id] = rw;
        y += row_h + 2;
    }
    s_list_scroll->setCanvasSize(s_list_scroll->getViewCoord().width, y);
}

static void apply_ping_to_row(const std::string& id) {
    auto it = s_rows.find(id);
    if (it == s_rows.end()) return;
    for (const ServerEntry& e : s_entries) {
        if (e.id != id) continue;
        if (it->second.line1) it->second.line1->setCaption(e.name + "   —   " + format_ping_line(e));
        return;
    }
}

static void update_button_states() {
    bool has_sel = false;
    bool can_join = false;
    for (const ServerEntry& e : s_entries) {
        if (e.id != s_selected_id) continue;
        has_sel = true;
        if (s_pinger && s_pinger->status(e.id) == ServerPinger::Status::Success) {
            can_join = true;
        }
        break;
    }
    if (s_btn_edit)   s_btn_edit->setEnabled(has_sel);
    if (s_btn_remove) s_btn_remove->setEnabled(has_sel);
    if (s_btn_join)   s_btn_join->setEnabled(can_join);
}

// ---------------------------------------------------------------------------
// Add / Edit / Remove / Direct / Join
// ---------------------------------------------------------------------------
static void on_add(MyGUI::Widget*) {
    if (!s_add_window) return;
    s_add_is_edit = false;
    s_add_name->setCaption("");
    s_add_addr->setCaption("");
    s_add_port->setCaption("7777");
    s_add_pw->setCaption("");
    s_add_err->setCaption("");
    s_add_window->setCaption("Add Server");
    s_add_window->setVisible(true);
    MyGUI::LayerManager::getInstance().upLayerItem(s_add_window);
}

static void on_edit(MyGUI::Widget*) {
    if (!s_add_window || s_selected_id.empty()) return;
    for (const ServerEntry& e : s_entries) {
        if (e.id != s_selected_id) continue;
        s_add_is_edit = true;
        s_add_name->setCaption(e.name);
        s_add_addr->setCaption(e.address);
        char pbuf[16];
        std::snprintf(pbuf, sizeof(pbuf), "%u", static_cast<unsigned>(e.port));
        s_add_port->setCaption(pbuf);
        s_add_pw->setCaption(e.password);
        s_add_err->setCaption("");
        s_add_window->setCaption("Edit Server");
        s_add_window->setVisible(true);
        MyGUI::LayerManager::getInstance().upLayerItem(s_add_window);
        return;
    }
}

static void on_add_ok(MyGUI::Widget*) {
    std::string name = s_add_name->getCaption();
    std::string addr = s_add_addr->getCaption();
    std::string portstr = s_add_port->getCaption();
    std::string pw = s_add_pw->getCaption();
    if (name.empty()) { s_add_err->setCaption("Name is required"); return; }
    if (addr.empty()) { s_add_err->setCaption("Address is required"); return; }
    int port = std::atoi(portstr.c_str());
    if (port <= 0 || port > 65535) { s_add_err->setCaption("Port must be 1..65535"); return; }

    if (s_add_is_edit) {
        for (ServerEntry& e : s_entries) {
            if (e.id != s_selected_id) continue;
            e.name = name; e.address = addr;
            e.port = static_cast<uint16_t>(port); e.password = pw;
            break;
        }
    } else {
        ServerEntry e;
        e.id = server_list_new_id();
        e.name = name; e.address = addr;
        e.port = static_cast<uint16_t>(port); e.password = pw;
        s_entries.push_back(e);
        s_selected_id = e.id;
    }
    server_list_save(s_entries);
    s_add_window->setVisible(false);
    rebuild_rows();
    start_all_pings();
    update_button_states();
}

static void on_add_cancel(MyGUI::Widget*) {
    if (s_add_window) s_add_window->setVisible(false);
}

static void on_remove(MyGUI::Widget*) {
    if (s_selected_id.empty()) return;
    for (auto it = s_entries.begin(); it != s_entries.end(); ++it) {
        if (it->id != s_selected_id) continue;
        KMP_LOG(std::string("[KenshiMP] browser: remove server '") + it->name + "'");
        s_entries.erase(it);
        break;
    }
    s_selected_id.clear();
    server_list_save(s_entries);
    rebuild_rows();
    update_button_states();
}

static void on_direct(MyGUI::Widget*) {
    // For A.3 MVP: same dialog as Add, but on OK we log+close instead of save.
    // Implemented as a degenerate Add flow with a flag would be overkill —
    // keep simple: reuse Add, the user can just delete afterwards.
    on_add(NULL);
}

static void on_join(MyGUI::Widget*) {
    for (const ServerEntry& e : s_entries) {
        if (e.id != s_selected_id) continue;
        char logbuf[256];
        std::snprintf(logbuf, sizeof(logbuf),
            "[KenshiMP] Join clicked: '%s' @ %s:%u",
            e.name.c_str(), e.address.c_str(), static_cast<unsigned>(e.port));
        KMP_LOG(logbuf);
        break;
    }
    server_browser_close();
}

} // anonymous namespace

} // namespace kmp
```

### Step 3: Register in `core/CMakeLists.txt`

Add `src/server_browser.cpp` to the `add_library(KenshiMP SHARED ...)` source list.

### Step 4: Build

```bash
make core 2>&1 | tail -5
```

Expected: clean build (warnings OK). If MyGUI scrollview/button skin names aren't recognised at runtime, that's a Kenshi-mod-data concern; the code compiles.

### Step 5: Commit

```bash
git add core/src/server_browser.h core/src/server_browser.cpp core/CMakeLists.txt
git commit -m "feat(core): server browser MyGUI window (list + ping + add/edit/remove)"
```

---

## Task 8: Wire into plugin + manual smoke test

**Files:**
- Modify: `core/src/ui.cpp` (replace `on_mp_menu_clicked` body)
- Modify: `core/src/player_sync.cpp` (tick + handle key press)
- Modify: `core/src/plugin.cpp` (init)

### Step 1: Plugin init + shutdown

In `core/src/plugin.cpp`, inside the `namespace kmp { ... }` forward-decl block at the top:

```cpp
    void server_browser_init();
    void server_browser_shutdown();
```

In `hooked_main_loop`'s "Defer subsystem init" block (around where `snapshot_uploader_glue_init();` is called), add:

```cpp
        kmp::server_browser_init();
```

### Step 2: Tick in `player_sync.cpp`

Near the other `extern void` declarations at the top of `player_sync.cpp`:

```cpp
extern void server_browser_tick(float dt);
```

In `player_sync_tick`, adjacent to `snapshot_uploader_glue_tick(dt);`, add:

```cpp
    server_browser_tick(dt);
```

### Step 3: Hook the Multiplayer button in `ui.cpp`

Add extern near the top (with the other `extern void ...` lines):

```cpp
extern void server_browser_open();
```

Find `on_mp_menu_clicked` (function that fires when the main-menu Multiplayer button is clicked). Replace its body:

```cpp
static void on_mp_menu_clicked(MyGUI::Widget* /*sender*/) {
    KMP_LOG("[KenshiMP] Multiplayer button → open server browser");
    server_browser_open();
}
```

### Step 4: Build + deploy

```bash
make deploy 2>&1 | tail -6
```

Expected: clean build and copy to Kenshi mods folder. If Kenshi is running, close it first (see A.2 lesson).

### Step 5: Manual smoke test

1. Launch Kenshi → title screen → click **Multiplayer**. Browser window appears empty.
2. Click **Add**. Fill Name="Local", Address="127.0.0.1", Port=7777, Password="". OK. Row appears.
3. Start the headless server in another shell:
   ```bash
   ./build_server/bin/Release/kenshi-mp-server-headless.exe
   ```
4. In the browser, click **Refresh**. Within ~1s the "Local" row updates to show "0/16   <ping>ms" and any description configured in `server/config.json`.
5. Select the row, click **Join**. The browser closes; the plugin log (`kenshimp_1.log`) contains `Join clicked: 'Local' @ 127.0.0.1:7777`.
6. Close Kenshi. Open `<Documents>\My Games\Kenshi\KenshiMP\servers.json`. Confirm the entry is saved.
7. Reopen Kenshi, click Multiplayer. The saved entry is still there.
8. Select, click **Remove**. Confirm the entry disappears and `servers.json` no longer contains it.

### Step 6: Commit

```bash
git add core/src/plugin.cpp core/src/player_sync.cpp core/src/ui.cpp
git commit -m "feat(core): wire server browser into plugin (Multiplayer button, tick)"
```

---

## Final verification

```bash
make test
```

All 9 unit test suites pass: packets (orig), store, upload, http-sidecar, zip, uploader, server-info-packets, server-list, server-pinger.

```bash
make test-e2e
```

Both e2e tests pass (snapshot upload + server-info ping).

Manual integration steps 1–8 pass on a fresh Kenshi launch.

---

## Self-review

- **Spec coverage**: "main-menu button opens browser" → Task 8. "Ping shows live data" → Tasks 3 (server) + 5/7 (client + UI). "Persistence" → Task 4. "Add/Edit/Remove/Direct/Join" → Task 7. "Password stored per-entry" → Task 4 (schema) + Task 7 (dialog). "Error paths" → Tasks 4 (corrupt file), 5 (timeouts, version mismatch), 7 (validation).
- **Placeholders**: none. Concrete code everywhere.
- **Type consistency**: `ServerEntry`, `ServerInfoReply`, `ServerPinger::Status::E`, `ReplyFields` — all named consistently across tasks.
- **Scope**: focused on UI + data + ping. Join is a log line; Plan A.4 wires it to the real runtime.

## Open implementation issues

1. If the MyGUI skin names `Kenshi_ScrollViewSkin`, `Kenshi_Button1Skin`, etc. don't exist in the game's MyGUI skin set, the widgets fail to instantiate. Task 7 compiles but Task 8's smoke test is what finds out. The existing `ui.cpp` uses `Kenshi_WindowCX`, `Kenshi_Button1Skin`, `Kenshi_EditBox`, `Kenshi_TextBoxEmptySkin` — we reuse those. `Kenshi_ScrollViewSkin` is new; if the game lacks it, substitute with a plain `Widget` + manual scrolling (note in the implementation).
2. The row-selection highlight uses `setStateSelected(true)`. If that doesn't visually do anything on the button skin, rows still select logically (click picks them up) but the highlight is invisible. Acceptable for MVP.
