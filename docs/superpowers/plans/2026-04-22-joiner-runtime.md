# Joiner Runtime (Plan A.4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the server browser's Join button to a real pipeline — WinHTTP download of the snapshot, miniz extract, `SaveManager::loadGame`, ENet handshake with password — and auto-transition into the host's world on `CONNECT_ACCEPT`. Connecting modal animates per stage; failures surface with retry/close.

**Architecture:** One DI state machine (`JoinerRuntime`) drives the pipeline; a glue file provides real bindings (WinHTTP, miniz, Kenshi, ENet, Win32 threads) while tests mock them all. Three helper modules (`snapshot_client`, `snapshot_extract`, `load_trigger`) each handle a single concern. Existing A.3 Connecting modal now reflects live pipeline state.

**Tech Stack:** Plugin side — C++11 / VS2010 v100 toolset (no std::thread, no std::filesystem), Win32 threads + WinHTTP + CreateDirectoryW. Shared — new `ConnectRequest.password` field. Server side — tiny `CONNECT_REQUEST` gate. Tests — C++17 / VS2022 v143, KMP_CHECK runtime asserts.

**Parent spec:** `docs/superpowers/specs/2026-04-22-joiner-runtime-design.md`

**Depends on:** Plans A.1 + A.2 + A.3 (all on main).

---

## File Structure

**New files (plugin side):**
- `core/src/snapshot_client.h` / `.cpp` — WinHTTP GET with progress cb
- `core/src/snapshot_extract.h` / `.cpp` — miniz unzip
- `core/src/load_trigger.h` / `.cpp` — `SaveManager::loadGame` + busy-poll
- `core/src/joiner_runtime.h` / `.cpp` — DI state machine
- `core/src/joiner_runtime_glue.h` / `.cpp` — Win32/WinHTTP/miniz/ENet/Kenshi real bindings

**Modified files:**
- `common/include/packets.h` — `ConnectRequest.password[64]`, `MAX_PASSWORD_LENGTH`
- `server/core/src/session.cpp` — validate password in `handle_connect_request`
- `core/src/server_browser.cpp` — `on_join` → `joiner_runtime_start`; FAILED modal buttons
- `core/src/player_sync.cpp` — `CONNECT_ACCEPT`/`CONNECT_REJECT` → forward to joiner_runtime
- `core/src/plugin.cpp` — `joiner_runtime_*_init/tick`
- `core/CMakeLists.txt` — register new sources, link `winhttp`

**New tests:**
- `tools/test_connect_request_password.cpp` — pack/unpack password field
- `tools/test_snapshot_extract.cpp` — zip → extract → byte-identical
- `tools/test_joiner_runtime.cpp` — 10 DI cases

**Responsibility split:**
- `snapshot_client` knows WinHTTP, nothing else.
- `snapshot_extract` knows miniz + filesystem, nothing else.
- `load_trigger` knows Kenshi `SaveManager` + `SaveFileSystem`, nothing else.
- `joiner_runtime` owns the state machine; zero direct knowledge of WinHTTP/miniz/Kenshi/ENet — all via Deps.
- `joiner_runtime_glue` is the only file that knows about real threads and connects the state machine to everything else.

---

## Task 1: `ConnectRequest.password` field + server gate

**Files:**
- Modify: `common/include/packets.h`
- Modify: `server/core/src/session.cpp` (`handle_connect_request`)
- Create: `tools/test_connect_request_password.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `Makefile` (`SNAPSHOT_UNIT_TESTS`)

Add a fixed-size password string to `ConnectRequest`, plus the server-side validation against `ServerConfig::password`. Round-trip test covers the new field.

- [ ] **Step 1: Write failing test**

Create `tools/test_connect_request_password.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <cstring>
#include "packets.h"
#include "serialization.h"

using namespace kmp;

static void test_password_roundtrip() {
    ConnectRequest orig;
    std::strncpy(orig.name, "Bob",          MAX_NAME_LENGTH - 1);
    std::strncpy(orig.model, "greenlander",  MAX_MODEL_LENGTH - 1);
    orig.is_host = 0;
    std::strncpy(orig.client_uuid, "some-uuid", sizeof(orig.client_uuid) - 1);
    std::strncpy(orig.password, "hunter2",  MAX_PASSWORD_LENGTH - 1);

    auto buf = pack(orig);
    ConnectRequest got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(std::strcmp(got.name,     "Bob") == 0);
    KMP_CHECK(std::strcmp(got.model,    "greenlander") == 0);
    KMP_CHECK(got.is_host == 0);
    KMP_CHECK(std::strcmp(got.password, "hunter2") == 0);
    printf("test_password_roundtrip OK\n");
}

static void test_empty_password_roundtrip() {
    ConnectRequest orig;
    std::strncpy(orig.name, "Alice", MAX_NAME_LENGTH - 1);
    // password left zero (default-constructed struct is memset to 0).

    auto buf = pack(orig);
    ConnectRequest got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(got.password[0] == '\0');
    printf("test_empty_password_roundtrip OK\n");
}

int main() {
    test_password_roundtrip();
    test_empty_password_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt` (match existing test-* targets):

```cmake
add_executable(test-connect-request-password test_connect_request_password.cpp)
target_link_libraries(test-connect-request-password PRIVATE kenshi-mp-common)
target_compile_features(test-connect-request-password PRIVATE cxx_std_17)
```

Extend `Makefile` `SNAPSHOT_UNIT_TESTS`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip test-snapshot-uploader \
                       test-server-info-packets test-server-list \
                       test-server-pinger test-connect-request-password
```

- [ ] **Step 2: Verify fails**

```bash
make test 2>&1 | tail -10
```

Expected: `MAX_PASSWORD_LENGTH` not declared / `password` not a member of `ConnectRequest`.

- [ ] **Step 3: Add `MAX_PASSWORD_LENGTH` + field to `common/include/packets.h`**

Near the top of the file (where `MAX_NAME_LENGTH`, `MAX_MODEL_LENGTH` etc. are defined — they're likely in `protocol.h` which is included by `packets.h`, but confirm with a grep):

If in `protocol.h`, add next to the other size constants:

```cpp
static const size_t MAX_PASSWORD_LENGTH = 64;
```

Otherwise put it at the top of `packets.h` before the first struct.

Then in the `ConnectRequest` struct (search for `struct ConnectRequest`), add the password field AFTER `client_uuid`:

```cpp
struct ConnectRequest {
    PacketHeader header;
    char         name[MAX_NAME_LENGTH];
    char         model[MAX_MODEL_LENGTH];
    uint8_t      is_host;
    char         client_uuid[64];
    char         password[MAX_PASSWORD_LENGTH];   // "" = no password provided

    ConnectRequest() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::CONNECT_REQUEST;
    }
};
```

- [ ] **Step 4: Build + run test**

```bash
make test 2>&1 | tail -10
```

Expected:
```
--- test-connect-request-password ---
test_password_roundtrip OK
test_empty_password_roundtrip OK
ALL PASS
```

- [ ] **Step 5: Add server-side password gate**

Find `handle_connect_request` in `server/core/src/session.cpp` (around line 160-250). After the `MAX_PLAYERS` check but BEFORE the `id` resolution block, insert:

```cpp
    // Password gate. If the server has a password configured and the
    // client's password doesn't match, reject.
    if (s_server_config && !s_server_config->password.empty()) {
        if (std::strcmp(req.password, s_server_config->password.c_str()) != 0) {
            ConnectReject reject;
            safe_strcpy(reject.reason, "wrong password");
            auto buf = pack(reject);
            relay_send_to(peer, buf.data(), buf.size(), true);
            enet_peer_disconnect_later(peer, 0);
            spdlog::info("CONNECT rejected: wrong password from {}",
                         peer->address.host);
            return;
        }
    }
```

- [ ] **Step 6: Build server**

```bash
make server-core 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 7: Commit**

```bash
git add common/include/packets.h server/core/src/session.cpp \
        tools/test_connect_request_password.cpp tools/CMakeLists.txt Makefile
# If MAX_PASSWORD_LENGTH was added to protocol.h, stage it too:
git add common/include/protocol.h
git commit -m "feat(common): ConnectRequest gains password field + server gate"
```

---

## Task 2: `snapshot_client` — WinHTTP download wrapper

**Files:**
- Create: `core/src/snapshot_client.h`
- Create: `core/src/snapshot_client.cpp`
- Modify: `core/CMakeLists.txt` (add source + link `winhttp`)

WinHTTP-based blocking download with progress callback. No unit test — requires a live HTTP server. Integration via manual smoke test in Task 9.

- [ ] **Step 1: Create `core/src/snapshot_client.h`**

```cpp
// snapshot_client.h — WinHTTP wrapper for fetching the host's snapshot
// zip. Blocking (caller runs it on a worker thread).
//
// Plugin side (v100 toolchain); cpp-httplib isn't usable so we use the
// native Windows HTTP stack directly.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace kmp {

/// Progress callback — called periodically from inside the blocking
/// download. Safe to write to std::atomic<uint64_t> from here. Must NOT
/// touch MyGUI or other main-thread resources (this runs on the worker).
typedef std::function<void(uint64_t bytes_done, uint64_t bytes_total)> SnapshotProgressCb;

enum SnapshotDownloadResult {
    SNAPSHOT_DOWNLOAD_OK              = 0,
    SNAPSHOT_DOWNLOAD_CONNECT_FAILED  = 1,
    SNAPSHOT_DOWNLOAD_HTTP_ERROR      = 2,   // any non-2xx
    SNAPSHOT_DOWNLOAD_WRITE_FAILED    = 3,
    SNAPSHOT_DOWNLOAD_CANCELLED       = 4,
    SNAPSHOT_DOWNLOAD_UNKNOWN         = 5,
};

/// Synchronously download `http://<host>:<port>/snapshot` to `out_path`.
/// Reports progress via `cb` (may be nullptr). Respects cancellation:
/// if `*cancel_flag` becomes non-zero, aborts ASAP and returns
/// CANCELLED. `http_status_out` (if non-null) receives the HTTP status
/// code (or 0 on connect failure).
SnapshotDownloadResult download_snapshot_blocking(
    const std::string& host,
    uint16_t           port,
    const std::string& out_path,
    const SnapshotProgressCb& cb,
    volatile long*     cancel_flag,
    int*               http_status_out);

} // namespace kmp
```

- [ ] **Step 2: Create `core/src/snapshot_client.cpp`**

```cpp
#include "snapshot_client.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "kmp_log.h"

namespace kmp {

namespace {

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()),
                                NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

} // namespace

SnapshotDownloadResult download_snapshot_blocking(
    const std::string& host,
    uint16_t           port,
    const std::string& out_path,
    const SnapshotProgressCb& cb,
    volatile long*     cancel_flag,
    int*               http_status_out) {

    if (http_status_out) *http_status_out = 0;

    HINTERNET hSession = WinHttpOpen(L"KenshiMP/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;

    std::wstring whost = utf8_to_wide(host);
    HINTERNET hConn = WinHttpConnect(hSession, whost.c_str(),
                                     static_cast<INTERNET_PORT>(port), 0);
    if (!hConn) { WinHttpCloseHandle(hSession); return SNAPSHOT_DOWNLOAD_CONNECT_FAILED; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", L"/snapshot",
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        0);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
                 return SNAPSHOT_DOWNLOAD_CONNECT_FAILED; }

    if (!WinHttpSendRequest(hReq,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;
    }

    if (!WinHttpReceiveResponse(hReq, NULL)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;
    }

    // Status code.
    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
    if (http_status_out) *http_status_out = static_cast<int>(status);
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_HTTP_ERROR;
    }

    // Content-Length (may be missing; we fall back to "unknown" progress).
    uint64_t total = 0;
    {
        DWORD cl = 0; DWORD cl_sz = sizeof(cl);
        if (WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &cl, &cl_sz, WINHTTP_NO_HEADER_INDEX)) {
            total = static_cast<uint64_t>(cl);
        }
    }

    FILE* out = NULL;
    if (fopen_s(&out, out_path.c_str(), "wb") != 0 || !out) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_WRITE_FAILED;
    }

    uint64_t done = 0;
    const DWORD CHUNK = 32 * 1024;
    std::vector<BYTE> buf(CHUNK);
    SnapshotDownloadResult rc = SNAPSHOT_DOWNLOAD_OK;

    while (true) {
        if (cancel_flag && InterlockedCompareExchange(cancel_flag, 0, 0) != 0) {
            rc = SNAPSHOT_DOWNLOAD_CANCELLED;
            break;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) { rc = SNAPSHOT_DOWNLOAD_UNKNOWN; break; }
        if (avail == 0) break;  // done

        DWORD read = 0;
        DWORD want = (avail > CHUNK) ? CHUNK : avail;
        if (!WinHttpReadData(hReq, buf.data(), want, &read) || read == 0) {
            rc = SNAPSHOT_DOWNLOAD_UNKNOWN; break;
        }
        if (std::fwrite(buf.data(), 1, read, out) != read) {
            rc = SNAPSHOT_DOWNLOAD_WRITE_FAILED; break;
        }
        done += read;
        if (cb) cb(done, total);
    }

    std::fclose(out);
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);

    if (rc == SNAPSHOT_DOWNLOAD_CANCELLED || rc != SNAPSHOT_DOWNLOAD_OK) {
        DeleteFileA(out_path.c_str());
    }
    return rc;
}

} // namespace kmp
```

- [ ] **Step 3: Register in `core/CMakeLists.txt`**

Add `src/snapshot_client.cpp` to the `add_library(KenshiMP SHARED ...)` source list.

Also link `winhttp`. Find the existing `target_link_libraries(KenshiMP PRIVATE ...)` call and add `winhttp`:

```cmake
target_link_libraries(KenshiMP PRIVATE winhttp)
```

(If no such call exists yet, add one after `target_include_directories`.)

- [ ] **Step 4: Build**

```bash
make core 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add core/src/snapshot_client.h core/src/snapshot_client.cpp core/CMakeLists.txt
git commit -m "feat(core): WinHTTP snapshot download wrapper with progress + cancel"
```

---

## Task 3: `snapshot_extract` — miniz unzip wrapper + test

**Files:**
- Create: `core/src/snapshot_extract.h`
- Create: `core/src/snapshot_extract.cpp`
- Create: `tools/test_snapshot_extract.cpp`
- Modify: `core/CMakeLists.txt` (add source)
- Modify: `tools/CMakeLists.txt` (add test target)
- Modify: `Makefile` (`SNAPSHOT_UNIT_TESTS`)

Given a zip file on disk and a destination directory, extract all entries. Mirror of `snapshot_zip` in reverse.

- [ ] **Step 1: Write failing test**

Create `tools/test_snapshot_extract.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "snapshot_extract.h"
#include "snapshot_zip.h"   // re-use A.2's zip_directory to build the fixture

namespace fs = std::filesystem;
using namespace kmp;

static fs::path make_tempdir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("kmp_extract_" + std::string(name)
        + "_" + std::to_string(std::time(nullptr)));
    fs::create_directories(p);
    return p;
}

static void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(contents.data(), contents.size());
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

static void test_roundtrip() {
    fs::path src = make_tempdir("src");
    write_file(src / "a.txt", "hello");
    write_file(src / "sub" / "b.bin", std::string("\x00\x01\x02", 3));
    write_file(src / "deep" / "nest" / "c.txt", "deep");

    std::vector<uint8_t> blob = kmp::zip_directory(src.string());
    KMP_CHECK(!blob.empty());

    fs::path zip_path = make_tempdir("zip") / "out.zip";
    {
        std::ofstream f(zip_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(blob.data()), blob.size());
    }

    fs::path dst = make_tempdir("dst");
    bool ok = kmp::extract_zip_to_dir(zip_path.string(), dst.string());
    KMP_CHECK(ok);

    KMP_CHECK(read_file(dst / "a.txt") == "hello");
    KMP_CHECK(read_file(dst / "sub" / "b.bin") == std::string("\x00\x01\x02", 3));
    KMP_CHECK(read_file(dst / "deep" / "nest" / "c.txt") == "deep");

    fs::remove_all(src);
    fs::remove_all(zip_path.parent_path());
    fs::remove_all(dst);
    printf("test_roundtrip OK\n");
}

static void test_missing_zip_returns_false() {
    fs::path dst = make_tempdir("dst-missing");
    bool ok = kmp::extract_zip_to_dir("C:/does/not/exist/kmp.zip", dst.string());
    KMP_CHECK(!ok);
    fs::remove_all(dst);
    printf("test_missing_zip_returns_false OK\n");
}

int main() {
    test_roundtrip();
    test_missing_zip_returns_false();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-snapshot-extract test_snapshot_extract.cpp
    ${CMAKE_SOURCE_DIR}/core/src/snapshot_extract.cpp
    ${CMAKE_SOURCE_DIR}/core/src/snapshot_zip.cpp
    ${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c)
target_include_directories(test-snapshot-extract PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/deps/miniz)
target_link_libraries(test-snapshot-extract PRIVATE kenshi-mp-common)
target_compile_features(test-snapshot-extract PRIVATE cxx_std_17)
```

Extend `Makefile`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip test-snapshot-uploader \
                       test-server-info-packets test-server-list \
                       test-server-pinger test-connect-request-password \
                       test-snapshot-extract
```

- [ ] **Step 2: Verify fails**

```bash
make test 2>&1 | tail -10
```

Expected: `snapshot_extract.h` not found.

- [ ] **Step 3: Create `core/src/snapshot_extract.h`**

```cpp
// snapshot_extract.h — miniz-based zip-to-dir extractor.
//
// Plugin side. Mirror of snapshot_zip in reverse. Creates any missing
// parent directories as it writes files.
#pragma once

#include <string>

namespace kmp {

/// Open `zip_path`, read every file entry, and write them under
/// `dst_dir` (creating parent directories as needed). Returns false
/// on any miniz or filesystem error.
///
/// Forward-slash separators in the archive map to backslash on disk.
bool extract_zip_to_dir(const std::string& zip_path,
                        const std::string& dst_dir);

} // namespace kmp
```

- [ ] **Step 4: Create `core/src/snapshot_extract.cpp`**

```cpp
#include "snapshot_extract.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "miniz.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kmp {

namespace {

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()),
                                NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

// Create a directory and all missing parents. Equivalent to
// std::filesystem::create_directories (not available on v100).
static bool create_dirs_w(const std::wstring& path) {
    if (path.empty()) return true;
    size_t start = 0;
    while (true) {
        size_t slash = path.find_first_of(L"\\/", start);
        std::wstring prefix = (slash == std::wstring::npos)
            ? path : path.substr(0, slash);
        if (!prefix.empty() && prefix.back() != L':') {
            if (!CreateDirectoryW(prefix.c_str(), NULL)) {
                DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) return false;
            }
        }
        if (slash == std::wstring::npos) return true;
        start = slash + 1;
    }
}

} // namespace

bool extract_zip_to_dir(const std::string& zip_path,
                        const std::string& dst_dir) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        return false;
    }

    std::wstring wdst = utf8_to_wide(dst_dir);
    if (!create_dirs_w(wdst)) {
        mz_zip_reader_end(&zip);
        return false;
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    bool ok = true;
    for (mz_uint i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) { ok = false; break; }
        if (stat.m_is_directory) continue;

        std::string entry_name = stat.m_filename;
        // Normalise to backslashes.
        for (size_t k = 0; k < entry_name.size(); ++k) {
            if (entry_name[k] == '/') entry_name[k] = '\\';
        }

        std::string out_path = dst_dir + "\\" + entry_name;
        std::wstring wout = utf8_to_wide(out_path);

        // Ensure parent directory exists.
        size_t last_sep = wout.find_last_of(L'\\');
        if (last_sep != std::wstring::npos) {
            std::wstring parent = wout.substr(0, last_sep);
            if (!create_dirs_w(parent)) { ok = false; break; }
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, out_path.c_str(), 0)) {
            ok = false;
            break;
        }
    }

    mz_zip_reader_end(&zip);
    return ok;
}

} // namespace kmp
```

- [ ] **Step 5: Register + build + test**

Add `src/snapshot_extract.cpp` to `core/CMakeLists.txt`'s `add_library(KenshiMP SHARED ...)` sources.

```bash
make core 2>&1 | tail -5
make test 2>&1 | tail -10
```

Expected:
```
--- test-snapshot-extract ---
test_roundtrip OK
test_missing_zip_returns_false OK
ALL PASS
```

- [ ] **Step 6: Commit**

```bash
git add core/src/snapshot_extract.h core/src/snapshot_extract.cpp \
        core/CMakeLists.txt \
        tools/test_snapshot_extract.cpp tools/CMakeLists.txt Makefile
git commit -m "feat(core): miniz unzip wrapper for joiner pipeline"
```

---

## Task 4: `load_trigger` — `SaveManager::loadGame` wrapper

**Files:**
- Create: `core/src/load_trigger.h`
- Create: `core/src/load_trigger.cpp`
- Modify: `core/CMakeLists.txt`

Thin wrapper mirroring `save_trigger`. No unit test — requires Kenshi.

- [ ] **Step 1: Create `core/src/load_trigger.h`**

```cpp
// load_trigger.h — Thin wrapper around Kenshi's SaveManager::loadGame
// and SaveFileSystem::busy() for the joiner pipeline.
//
// Mirrors save_trigger (A.2) in reverse.
#pragma once

#include <string>

namespace kmp {

/// Kick off a load of the given slot. Returns false if the SaveManager
/// singleton isn't available or Kenshi returns an error. Kenshi's load
/// is asynchronous — poll load_trigger_is_busy() until it returns false.
bool load_trigger_start(const std::string& slot_name);

/// True while Kenshi's load worker is running. Mirrors
/// SaveFileSystem::busy() with a 4-second post-start grace period
/// (same pattern as save_trigger, since loadGame also only queues).
bool load_trigger_is_busy();

/// Same resolver as save_trigger — SaveManager's userSavePath or
/// localSavePath.
std::string load_trigger_resolve_slot_path(const std::string& slot_name);

} // namespace kmp
```

- [ ] **Step 2: Create `core/src/load_trigger.cpp`**

```cpp
#include "load_trigger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>

#include <kenshi/SaveFileSystem.h>
#include <kenshi/SaveManager.h>

#include "kmp_log.h"
#include "save_trigger.h"   // re-use resolve_slot_path

namespace kmp {

static ULONGLONG s_load_started_ms = 0;

bool load_trigger_start(const std::string& slot_name) {
    SaveManager* sm = SaveManager::getSingleton();
    if (!sm) {
        KMP_LOG("[KenshiMP] load_trigger: SaveManager singleton null");
        return false;
    }
    // Same location resolution as save_trigger — userSavePath or localSavePath.
    const std::string& loc = sm->userSavePath.empty() ? sm->localSavePath
                                                       : sm->userSavePath;
    int rc = sm->loadGame(loc, slot_name);
    s_load_started_ms = GetTickCount64();
    char msg[256];
    _snprintf(msg, sizeof(msg),
        "[KenshiMP] load_trigger: loadGame(loc='%s', name='%s') returned %d",
        loc.c_str(), slot_name.c_str(), rc);
    KMP_LOG(msg);
    // Kenshi returns non-zero on synchronous refusal; if it returns 0 the
    // load is queued and will run on the main thread.
    return rc >= 0;
}

bool load_trigger_is_busy() {
    const ULONGLONG kGraceMs = 4000;
    ULONGLONG now = GetTickCount64();
    bool in_grace = (s_load_started_ms > 0) &&
                    (now - s_load_started_ms < kGraceMs);
    SaveFileSystem* sfs = SaveFileSystem::getSingleton();
    bool worker_busy = sfs ? sfs->busy() : false;
    return in_grace || worker_busy;
}

std::string load_trigger_resolve_slot_path(const std::string& slot_name) {
    // Re-use save_trigger's resolver — same Kenshi save root.
    return save_trigger_resolve_slot_path(slot_name);
}

} // namespace kmp
```

- [ ] **Step 3: Register + build**

Add `src/load_trigger.cpp` to `core/CMakeLists.txt` source list.

```bash
make core 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add core/src/load_trigger.h core/src/load_trigger.cpp core/CMakeLists.txt
git commit -m "feat(core): load_trigger wraps SaveManager::loadGame for joiner"
```

---

## Task 5: `JoinerRuntime` state machine + unit tests

**Files:**
- Create: `core/src/joiner_runtime.h`
- Create: `core/src/joiner_runtime.cpp`
- Create: `tools/test_joiner_runtime.cpp`
- Modify: `core/CMakeLists.txt`
- Modify: `tools/CMakeLists.txt`
- Modify: `Makefile`

The DI state machine. Largest single task in this plan. All external dependencies injected; tests run synchronously with mocks.

- [ ] **Step 1: Write failing tests**

Create `tools/test_joiner_runtime.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "joiner_runtime.h"
#include "server_list.h"

using namespace kmp;

namespace {

struct Mock {
    // Download
    bool download_started      = false;
    bool download_poll_done    = false;
    bool download_succeeded    = true;
    uint64_t  bytes_done       = 0;
    uint64_t  bytes_total      = 1024 * 1024;
    bool download_cancelled    = false;

    // Extract
    bool extract_started       = false;
    bool extract_poll_done     = false;
    bool extract_succeeded     = true;

    // Load
    bool load_triggered        = false;
    bool load_trigger_result   = true;
    bool load_still_busy       = true;

    // Connect
    bool connect_called        = false;
    bool connect_result        = true;
    bool send_request_called   = false;
    std::string last_password;

    // Clock
    float now = 0.0f;

    // Resolve
    std::string resolved_path = "C:/tmp/KMP";
};

static JoinerRuntime::Deps make_deps(Mock* m) {
    JoinerRuntime::Deps d;
    d.start_download = [m](const std::string&, uint16_t, const std::string&) {
        m->download_started = true;
    };
    d.poll_download = [m](uint64_t& done, uint64_t& total) -> bool {
        done  = m->bytes_done;
        total = m->bytes_total;
        return m->download_poll_done;
    };
    d.cancel_download = [m]() { m->download_cancelled = true; };
    d.download_succeeded = [m]() { return m->download_succeeded; };

    d.start_extract = [m](const std::string&, const std::string&) {
        m->extract_started = true;
    };
    d.poll_extract = [m](bool& ok) -> bool {
        ok = m->extract_succeeded;
        return m->extract_poll_done;
    };

    d.trigger_load = [m](const std::string&, const std::string&) -> bool {
        m->load_triggered = true;
        return m->load_trigger_result;
    };
    d.is_load_busy = [m]() -> bool { return m->load_still_busy; };

    d.connect_enet = [m](const std::string&, uint16_t) -> bool {
        m->connect_called = true;
        return m->connect_result;
    };
    d.send_connect_request = [m](const std::string& pw) -> bool {
        m->send_request_called = true;
        m->last_password = pw;
        return true;
    };
    d.disconnect_enet = []() {};

    d.now_seconds = [m]() { return m->now; };
    d.resolve_slot_path = [m](const std::string&) { return m->resolved_path; };

    return d;
}

static ServerEntry make_entry() {
    ServerEntry e;
    e.id = "abcd1234"; e.name = "Local";
    e.address = "127.0.0.1"; e.port = 7777;
    e.password = "hunter2"; e.last_joined_ms = 0;
    return e;
}

static void drive_until(JoinerRuntime& r, Mock& m, JoinerRuntime::State::E target) {
    int safety = 10000;
    while (r.state() != target && safety-- > 0) {
        r.tick(0.016f);
        m.now += 0.016f;
    }
    KMP_CHECK(safety > 0);
}

static void test_happy_path() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    KMP_CHECK(r.state() == JoinerRuntime::State::Downloading);
    KMP_CHECK(m.download_started);

    // Simulate download progress then completion.
    m.bytes_done = 512 * 1024;
    r.tick(0.016f);
    m.bytes_done = 1024 * 1024; m.download_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    KMP_CHECK(m.extract_started);

    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadTrigger);
    // Immediately transitions LoadTrigger → LoadWait if trigger_load ok.
    r.tick(0.016f); m.now += 0.016f;
    KMP_CHECK(r.state() == JoinerRuntime::State::LoadWait);
    KMP_CHECK(m.load_triggered);

    // Load finishes.
    m.now += 5.0f;  // past grace
    m.load_still_busy = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::EnetConnect);

    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::AwaitAccept);
    KMP_CHECK(m.connect_called);
    KMP_CHECK(m.send_request_called);
    KMP_CHECK(m.last_password == "hunter2");

    r.on_connect_accept(123);
    KMP_CHECK(r.state() == JoinerRuntime::State::Done);

    printf("test_happy_path OK\n");
}

static void test_download_error_times_out() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    // Never mark download done; let clock advance > 120 s.
    for (int i = 0; i < 200; ++i) {
        m.now += 1.0f;
        r.tick(1.0f);
        if (r.state() == JoinerRuntime::State::Failed) break;
    }
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("Download timed out") != std::string::npos);
    printf("test_download_error_times_out OK\n");
}

static void test_download_failed_reports_error() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true;
    m.download_succeeded = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    printf("test_download_failed_reports_error OK\n");
}

static void test_extract_error() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true;
    m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    m.extract_succeeded = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("corrupt") != std::string::npos);
    printf("test_extract_error OK\n");
}

static void test_load_refused() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadTrigger);
    m.load_trigger_result = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    printf("test_load_refused OK\n");
}

static void test_load_timeout() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    for (int i = 0; i < 200; ++i) {
        m.now += 1.0f;
        r.tick(1.0f);
        if (r.state() == JoinerRuntime::State::Failed) break;
    }
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("Load timed out") != std::string::npos);
    printf("test_load_timeout OK\n");
}

static void test_connect_enet_fails() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    m.now += 5.0f; m.load_still_busy = false;
    drive_until(r, m, JoinerRuntime::State::EnetConnect);
    m.connect_result = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    printf("test_connect_enet_fails OK\n");
}

static void test_accept_timeout() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    m.now += 5.0f; m.load_still_busy = false;
    drive_until(r, m, JoinerRuntime::State::AwaitAccept);
    for (int i = 0; i < 40; ++i) { m.now += 1.0f; r.tick(1.0f);
        if (r.state() == JoinerRuntime::State::Failed) break; }
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("didn't respond") != std::string::npos);
    printf("test_accept_timeout OK\n");
}

static void test_reject_password() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    m.now += 5.0f; m.load_still_busy = false;
    drive_until(r, m, JoinerRuntime::State::AwaitAccept);
    r.on_connect_reject("wrong password");
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("Wrong password") != std::string::npos);
    printf("test_reject_password OK\n");
}

static void test_cancel_during_download() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    KMP_CHECK(r.state() == JoinerRuntime::State::Downloading);
    r.cancel();
    KMP_CHECK(r.state() == JoinerRuntime::State::Cancelled);
    KMP_CHECK(m.download_cancelled);
    printf("test_cancel_during_download OK\n");
}

} // namespace

int main() {
    test_happy_path();
    test_download_error_times_out();
    test_download_failed_reports_error();
    test_extract_error();
    test_load_refused();
    test_load_timeout();
    test_connect_enet_fails();
    test_accept_timeout();
    test_reject_password();
    test_cancel_during_download();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-joiner-runtime test_joiner_runtime.cpp
    ${CMAKE_SOURCE_DIR}/core/src/joiner_runtime.cpp
    ${CMAKE_SOURCE_DIR}/core/src/server_list.cpp)
target_include_directories(test-joiner-runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(test-joiner-runtime PRIVATE kenshi-mp-common)
target_compile_features(test-joiner-runtime PRIVATE cxx_std_17)
```

Extend `Makefile`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip test-snapshot-uploader \
                       test-server-info-packets test-server-list \
                       test-server-pinger test-connect-request-password \
                       test-snapshot-extract test-joiner-runtime
```

- [ ] **Step 2: Verify fails**

```bash
make test 2>&1 | tail -10
```

Expected: `joiner_runtime.h` not found.

- [ ] **Step 3: Create `core/src/joiner_runtime.h`**

```cpp
// joiner_runtime.h — State machine that drives the full joiner pipeline
// (download → extract → load → connect). All external calls are DI'd as
// std::function so tests run synchronously without real threads, HTTP,
// Kenshi, or ENet.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace kmp {

struct ServerEntry;

class JoinerRuntime {
public:
    struct State { enum E {
        Idle,
        Downloading,
        Extracting,
        LoadTrigger,    // one-tick transitional state (calls trigger_load)
        LoadWait,       // polling is_load_busy
        EnetConnect,    // one-tick (calls connect_enet + send_connect_request)
        AwaitAccept,
        Done,
        Cancelled,
        Failed,
    }; };

    struct Deps {
        // Download (async on a worker thread in production). start_download
        // returns immediately; poll_download returns true when the worker
        // has finished (success OR failure — check download_succeeded).
        std::function<void(const std::string& host, uint16_t port, const std::string& out_path)> start_download;
        std::function<bool(uint64_t& bytes_done, uint64_t& bytes_total)>                          poll_download;
        std::function<void()>                                                                     cancel_download;
        std::function<bool()>                                                                     download_succeeded;

        // Extract (also async).
        std::function<void(const std::string& zip_path, const std::string& dst_dir)>              start_extract;
        std::function<bool(bool& ok)>                                                             poll_extract;

        // Load (synchronous trigger; async completion via is_load_busy).
        std::function<bool(const std::string& location, const std::string& slot)>                 trigger_load;
        std::function<bool()>                                                                     is_load_busy;

        // Connect.
        std::function<bool(const std::string& host, uint16_t port)>                               connect_enet;
        std::function<bool(const std::string& password)>                                          send_connect_request;
        std::function<void()>                                                                     disconnect_enet;

        // Infrastructure.
        std::function<float()>                                                                    now_seconds;
        std::function<std::string(const std::string& slot)>                                       resolve_slot_path;
    };

    explicit JoinerRuntime(Deps deps);

    void start(const ServerEntry& entry);
    void cancel();
    void tick(float dt);
    void on_connect_accept(uint32_t player_id);
    void on_connect_reject(const std::string& reason);

    State::E           state() const;
    const std::string& last_error() const;
    std::string        progress_text() const;
    std::string        stage_label() const;   // "Downloading", "Extracting", ...

private:
    Deps m_deps;
    State::E m_state;
    std::string m_host;
    uint16_t    m_port;
    std::string m_password;
    std::string m_slot;
    std::string m_zip_path;
    std::string m_slot_dir;

    float  m_start_t;            // time of last state transition (for timeouts)
    float  m_enter_download_t;
    float  m_enter_load_t;
    float  m_enter_await_t;

    uint64_t m_bytes_done;
    uint64_t m_bytes_total;

    std::string m_error;

    void go_failed(const std::string& msg);
    void tick_download();
    void tick_extract();
    void tick_load_wait();
    void tick_await_accept();
};

} // namespace kmp
```

- [ ] **Step 4: Create `core/src/joiner_runtime.cpp`**

```cpp
#include "joiner_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "server_list.h"

namespace kmp {

static const float kDownloadTimeoutSec = 120.0f;
static const float kLoadTimeoutSec     = 120.0f;
static const float kAcceptTimeoutSec   = 30.0f;

JoinerRuntime::JoinerRuntime(Deps deps)
    : m_deps(deps), m_state(State::Idle),
      m_port(0),
      m_start_t(0.0f), m_enter_download_t(0.0f),
      m_enter_load_t(0.0f), m_enter_await_t(0.0f),
      m_bytes_done(0), m_bytes_total(0) {}

void JoinerRuntime::start(const ServerEntry& entry) {
    m_host = entry.address;
    m_port = entry.port;
    m_password = entry.password;
    m_slot = std::string("KMP_") + entry.id;
    m_slot_dir = m_deps.resolve_slot_path(m_slot);
    m_zip_path = m_slot_dir + ".zip";
    m_error.clear();
    m_bytes_done = 0;
    m_bytes_total = 0;

    m_state = State::Downloading;
    m_enter_download_t = m_deps.now_seconds();
    m_deps.start_download(m_host, m_port, m_zip_path);
}

void JoinerRuntime::cancel() {
    switch (m_state) {
    case State::Downloading:
        m_deps.cancel_download();
        m_state = State::Cancelled;
        break;
    case State::Extracting:
    case State::LoadTrigger:
    case State::LoadWait:
    case State::EnetConnect:
    case State::AwaitAccept:
        m_deps.disconnect_enet();
        m_state = State::Cancelled;
        break;
    default: break;
    }
}

void JoinerRuntime::tick(float /*dt*/) {
    switch (m_state) {
    case State::Downloading:   tick_download();     break;
    case State::Extracting:    tick_extract();      break;
    case State::LoadTrigger: {
        if (!m_deps.trigger_load(m_deps.resolve_slot_path(""), m_slot)) {
            go_failed("Kenshi refused to load the world");
            break;
        }
        m_state = State::LoadWait;
        m_enter_load_t = m_deps.now_seconds();
        break;
    }
    case State::LoadWait:      tick_load_wait();    break;
    case State::EnetConnect: {
        if (!m_deps.connect_enet(m_host, m_port)) {
            go_failed("Cannot open connection");
            break;
        }
        if (!m_deps.send_connect_request(m_password)) {
            go_failed("Cannot send ConnectRequest");
            break;
        }
        m_state = State::AwaitAccept;
        m_enter_await_t = m_deps.now_seconds();
        break;
    }
    case State::AwaitAccept:   tick_await_accept(); break;
    default: break;
    }
}

void JoinerRuntime::tick_download() {
    uint64_t done = 0, total = 0;
    bool finished = m_deps.poll_download(done, total);
    m_bytes_done = done;
    m_bytes_total = total;
    if (finished) {
        if (!m_deps.download_succeeded()) {
            go_failed("Download failed");
            return;
        }
        m_deps.start_extract(m_zip_path, m_slot_dir);
        m_state = State::Extracting;
        return;
    }
    if (m_deps.now_seconds() - m_enter_download_t > kDownloadTimeoutSec) {
        m_deps.cancel_download();
        go_failed("Download timed out");
    }
}

void JoinerRuntime::tick_extract() {
    bool ok = false;
    if (!m_deps.poll_extract(ok)) return;
    if (!ok) { go_failed("Extracted world is corrupt"); return; }
    m_state = State::LoadTrigger;
}

void JoinerRuntime::tick_load_wait() {
    if (m_deps.is_load_busy()) {
        if (m_deps.now_seconds() - m_enter_load_t > kLoadTimeoutSec) {
            go_failed("Load timed out");
        }
        return;
    }
    m_state = State::EnetConnect;
}

void JoinerRuntime::tick_await_accept() {
    if (m_deps.now_seconds() - m_enter_await_t > kAcceptTimeoutSec) {
        m_deps.disconnect_enet();
        go_failed("Server didn't respond");
    }
}

void JoinerRuntime::on_connect_accept(uint32_t /*player_id*/) {
    if (m_state != State::AwaitAccept) return;
    m_state = State::Done;
}

void JoinerRuntime::on_connect_reject(const std::string& reason) {
    if (m_state != State::AwaitAccept) return;
    if (reason.find("wrong password") != std::string::npos) {
        go_failed("Wrong password");
    } else {
        go_failed(std::string("Rejected: ") + reason);
    }
    m_deps.disconnect_enet();
}

void JoinerRuntime::go_failed(const std::string& msg) {
    m_error = msg;
    m_state = State::Failed;
}

JoinerRuntime::State::E JoinerRuntime::state() const { return m_state; }
const std::string& JoinerRuntime::last_error() const { return m_error; }

std::string JoinerRuntime::stage_label() const {
    switch (m_state) {
    case State::Downloading: return "Downloading world";
    case State::Extracting:  return "Extracting";
    case State::LoadTrigger:
    case State::LoadWait:    return "Loading world";
    case State::EnetConnect:
    case State::AwaitAccept: return "Connecting";
    default:                 return "";
    }
}

std::string JoinerRuntime::progress_text() const {
    if (m_state == State::Downloading && m_bytes_total > 0) {
        float mb_done  = static_cast<float>(m_bytes_done)  / (1024.0f * 1024.0f);
        float mb_total = static_cast<float>(m_bytes_total) / (1024.0f * 1024.0f);
        char buf[64];
        _snprintf(buf, sizeof(buf), "%.1f / %.1f MB", mb_done, mb_total);
        return buf;
    }
    return "";
}

} // namespace kmp
```

- [ ] **Step 5: Register + build + test**

Add `src/joiner_runtime.cpp` to `core/CMakeLists.txt` source list.

```bash
make core 2>&1 | tail -5
make test 2>&1 | tail -20
```

Expected:
```
--- test-joiner-runtime ---
test_happy_path OK
test_download_error_times_out OK
test_download_failed_reports_error OK
test_extract_error OK
test_load_refused OK
test_load_timeout OK
test_connect_enet_fails OK
test_accept_timeout OK
test_reject_password OK
test_cancel_during_download OK
ALL PASS
```

- [ ] **Step 6: Commit**

```bash
git add core/src/joiner_runtime.h core/src/joiner_runtime.cpp \
        core/CMakeLists.txt \
        tools/test_joiner_runtime.cpp tools/CMakeLists.txt Makefile
git commit -m "feat(core): JoinerRuntime state machine with DI + 10 unit tests"
```

---

## Task 6: `joiner_runtime_glue` — real Win32/WinHTTP/Kenshi/ENet bindings

**Files:**
- Create: `core/src/joiner_runtime_glue.h`
- Create: `core/src/joiner_runtime_glue.cpp`
- Modify: `core/CMakeLists.txt`

Wires `JoinerRuntime::Deps` to real implementations. Uses Win32 `CreateThread` + `InterlockedExchange` for async download/extract (plugin toolchain has no `std::thread`/`std::atomic`).

- [ ] **Step 1: Create `core/src/joiner_runtime_glue.h`**

```cpp
// joiner_runtime_glue.h — Real Win32 / WinHTTP / miniz / ENet / Kenshi
// bindings for JoinerRuntime. Only file in the plugin that spans all
// those worlds.
#pragma once

#include <string>

namespace kmp {

struct ServerEntry;

void joiner_runtime_glue_init();
void joiner_runtime_glue_shutdown();

void joiner_runtime_glue_start(const ServerEntry& entry);
void joiner_runtime_glue_cancel();
void joiner_runtime_glue_tick(float dt);
void joiner_runtime_glue_on_connect_accept(uint32_t player_id);
void joiner_runtime_glue_on_connect_reject(const std::string& reason);

int         joiner_runtime_glue_state_int();
std::string joiner_runtime_glue_stage_label();
std::string joiner_runtime_glue_progress_text();
std::string joiner_runtime_glue_last_error();

} // namespace kmp
```

- [ ] **Step 2: Create `core/src/joiner_runtime_glue.cpp`**

```cpp
#include "joiner_runtime_glue.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "joiner_runtime.h"
#include "kmp_log.h"
#include "load_trigger.h"
#include "server_list.h"
#include "snapshot_client.h"
#include "snapshot_extract.h"

// Forward-decls from client.cpp.
namespace kmp {
    extern bool client_connect(const char* host, uint16_t port);
    extern void client_disconnect();
    extern void client_send_reliable(const uint8_t* data, size_t length);
}

namespace kmp {

namespace {

// ---- Download worker state ----
struct DownloadJob {
    HANDLE        thread;
    volatile LONG cancel_flag;
    volatile LONG done;          // 0 running, 1 finished
    volatile LONG succeeded;     // 0 failed, 1 ok
    volatile LONG bytes_done_hi, bytes_done_lo;
    volatile LONG bytes_total_hi, bytes_total_lo;
    std::string   host; uint16_t port;
    std::string   out_path;

    DownloadJob() : thread(NULL), cancel_flag(0), done(0), succeeded(0),
                    bytes_done_hi(0), bytes_done_lo(0),
                    bytes_total_hi(0), bytes_total_lo(0), port(0) {}
};

static DWORD WINAPI download_thread_proc(LPVOID param) {
    DownloadJob* job = reinterpret_cast<DownloadJob*>(param);
    SnapshotProgressCb cb = [job](uint64_t done, uint64_t total) {
        InterlockedExchange(&job->bytes_done_hi,  static_cast<LONG>(done  >> 32));
        InterlockedExchange(&job->bytes_done_lo,  static_cast<LONG>(done  & 0xFFFFFFFF));
        InterlockedExchange(&job->bytes_total_hi, static_cast<LONG>(total >> 32));
        InterlockedExchange(&job->bytes_total_lo, static_cast<LONG>(total & 0xFFFFFFFF));
    };
    int http = 0;
    SnapshotDownloadResult rc = download_snapshot_blocking(
        job->host, job->port, job->out_path, cb, &job->cancel_flag, &http);
    InterlockedExchange(&job->succeeded, rc == SNAPSHOT_DOWNLOAD_OK ? 1 : 0);
    InterlockedExchange(&job->done, 1);
    return 0;
}

// ---- Extract worker state ----
struct ExtractJob {
    HANDLE        thread;
    volatile LONG done;
    volatile LONG succeeded;
    std::string   zip_path;
    std::string   dst_dir;

    ExtractJob() : thread(NULL), done(0), succeeded(0) {}
};

static DWORD WINAPI extract_thread_proc(LPVOID param) {
    ExtractJob* job = reinterpret_cast<ExtractJob*>(param);
    bool ok = extract_zip_to_dir(job->zip_path, job->dst_dir);
    InterlockedExchange(&job->succeeded, ok ? 1 : 0);
    InterlockedExchange(&job->done, 1);
    return 0;
}

// ---- Globals ----
static std::unique_ptr<JoinerRuntime> s_runtime;
static std::unique_ptr<DownloadJob>   s_dl_job;
static std::unique_ptr<ExtractJob>    s_ex_job;

static float clock_seconds() {
    static LARGE_INTEGER freq, t0;
    static bool init = false;
    if (!init) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); init = true; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return static_cast<float>(now.QuadPart - t0.QuadPart) / static_cast<float>(freq.QuadPart);
}

static void start_download_bg(const std::string& host, uint16_t port,
                              const std::string& out_path) {
    if (s_dl_job && s_dl_job->thread) {
        WaitForSingleObject(s_dl_job->thread, INFINITE);
        CloseHandle(s_dl_job->thread);
    }
    s_dl_job.reset(new DownloadJob());
    s_dl_job->host = host; s_dl_job->port = port; s_dl_job->out_path = out_path;
    s_dl_job->thread = CreateThread(NULL, 0, download_thread_proc,
                                    s_dl_job.get(), 0, NULL);
}

static bool poll_download_bg(uint64_t& done, uint64_t& total) {
    if (!s_dl_job) return false;
    uint64_t dhi = static_cast<uint64_t>(InterlockedCompareExchange(&s_dl_job->bytes_done_hi, 0, 0));
    uint64_t dlo = static_cast<uint64_t>(static_cast<uint32_t>(InterlockedCompareExchange(&s_dl_job->bytes_done_lo, 0, 0)));
    uint64_t thi = static_cast<uint64_t>(InterlockedCompareExchange(&s_dl_job->bytes_total_hi, 0, 0));
    uint64_t tlo = static_cast<uint64_t>(static_cast<uint32_t>(InterlockedCompareExchange(&s_dl_job->bytes_total_lo, 0, 0)));
    done  = (dhi << 32) | dlo;
    total = (thi << 32) | tlo;
    return InterlockedCompareExchange(&s_dl_job->done, 0, 0) != 0;
}

static void cancel_download_bg() {
    if (!s_dl_job) return;
    InterlockedExchange(&s_dl_job->cancel_flag, 1);
}

static bool download_succeeded_bg() {
    if (!s_dl_job) return false;
    return InterlockedCompareExchange(&s_dl_job->succeeded, 0, 0) != 0;
}

static void start_extract_bg(const std::string& zip_path, const std::string& dst_dir) {
    if (s_ex_job && s_ex_job->thread) {
        WaitForSingleObject(s_ex_job->thread, INFINITE);
        CloseHandle(s_ex_job->thread);
    }
    s_ex_job.reset(new ExtractJob());
    s_ex_job->zip_path = zip_path; s_ex_job->dst_dir = dst_dir;
    s_ex_job->thread = CreateThread(NULL, 0, extract_thread_proc,
                                    s_ex_job.get(), 0, NULL);
}

static bool poll_extract_bg(bool& ok) {
    if (!s_ex_job) return false;
    if (InterlockedCompareExchange(&s_ex_job->done, 0, 0) == 0) return false;
    ok = InterlockedCompareExchange(&s_ex_job->succeeded, 0, 0) != 0;
    return true;
}

static bool connect_enet_real(const std::string& host, uint16_t port) {
    return client_connect(host.c_str(), port);
}

static bool send_connect_request_real(const std::string& password) {
    ConnectRequest req;
    // Name/model/uuid are filled by ui.cpp's do_connect path today; we
    // replicate the minimum here for the joiner flow.
    std::strncpy(req.name,  "Player",      MAX_NAME_LENGTH - 1);
    std::strncpy(req.model, "greenlander", MAX_MODEL_LENGTH - 1);
    req.is_host = 0;
    // UUID from client_identity.
    extern const char* client_identity_get_uuid();
    std::strncpy(req.client_uuid, client_identity_get_uuid(),
                 sizeof(req.client_uuid) - 1);
    std::strncpy(req.password, password.c_str(), MAX_PASSWORD_LENGTH - 1);

    std::vector<uint8_t> buf = pack(req);
    client_send_reliable(buf.data(), buf.size());
    return true;
}

} // namespace

void joiner_runtime_glue_init() {
    JoinerRuntime::Deps d;
    d.start_download     = [](const std::string& h, uint16_t p, const std::string& out) { start_download_bg(h, p, out); };
    d.poll_download      = [](uint64_t& done, uint64_t& total) { return poll_download_bg(done, total); };
    d.cancel_download    = []() { cancel_download_bg(); };
    d.download_succeeded = []() { return download_succeeded_bg(); };
    d.start_extract      = [](const std::string& z, const std::string& dst) { start_extract_bg(z, dst); };
    d.poll_extract       = [](bool& ok) { return poll_extract_bg(ok); };
    d.trigger_load       = [](const std::string& /*loc*/, const std::string& slot) { return load_trigger_start(slot); };
    d.is_load_busy       = []() { return load_trigger_is_busy(); };
    d.connect_enet       = [](const std::string& h, uint16_t p) { return connect_enet_real(h, p); };
    d.send_connect_request = [](const std::string& pw) { return send_connect_request_real(pw); };
    d.disconnect_enet    = []() { client_disconnect(); };
    d.now_seconds        = []() { return clock_seconds(); };
    d.resolve_slot_path  = [](const std::string& slot) { return load_trigger_resolve_slot_path(slot); };
    s_runtime.reset(new JoinerRuntime(d));
}

void joiner_runtime_glue_shutdown() {
    if (s_dl_job && s_dl_job->thread) {
        cancel_download_bg();
        WaitForSingleObject(s_dl_job->thread, 2000);
        CloseHandle(s_dl_job->thread);
        s_dl_job->thread = NULL;
    }
    if (s_ex_job && s_ex_job->thread) {
        WaitForSingleObject(s_ex_job->thread, 2000);
        CloseHandle(s_ex_job->thread);
        s_ex_job->thread = NULL;
    }
    s_dl_job.reset();
    s_ex_job.reset();
    s_runtime.reset();
}

void joiner_runtime_glue_start(const ServerEntry& entry) {
    if (!s_runtime) return;
    s_runtime->start(entry);
}

void joiner_runtime_glue_cancel() {
    if (!s_runtime) return;
    s_runtime->cancel();
}

void joiner_runtime_glue_tick(float dt) {
    if (!s_runtime) return;
    s_runtime->tick(dt);
}

void joiner_runtime_glue_on_connect_accept(uint32_t pid) {
    if (!s_runtime) return;
    s_runtime->on_connect_accept(pid);
}

void joiner_runtime_glue_on_connect_reject(const std::string& reason) {
    if (!s_runtime) return;
    s_runtime->on_connect_reject(reason);
}

int joiner_runtime_glue_state_int() {
    if (!s_runtime) return -1;
    return static_cast<int>(s_runtime->state());
}

std::string joiner_runtime_glue_stage_label() {
    if (!s_runtime) return std::string();
    return s_runtime->stage_label();
}

std::string joiner_runtime_glue_progress_text() {
    if (!s_runtime) return std::string();
    return s_runtime->progress_text();
}

std::string joiner_runtime_glue_last_error() {
    if (!s_runtime) return std::string();
    return s_runtime->last_error();
}

} // namespace kmp
```

- [ ] **Step 3: Register + build**

Add `src/joiner_runtime_glue.cpp` to `core/CMakeLists.txt` source list.

```bash
make core 2>&1 | tail -5
```

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add core/src/joiner_runtime_glue.h core/src/joiner_runtime_glue.cpp core/CMakeLists.txt
git commit -m "feat(core): JoinerRuntime real-world glue (WinHTTP + miniz + Kenshi threads)"
```

---

## Task 7: Wire Join button + CONNECT_ACCEPT/REJECT + plugin init/tick

**Files:**
- Modify: `core/src/server_browser.cpp` — replace placeholder modal logic with `joiner_runtime_glue_*` driven modal
- Modify: `core/src/player_sync.cpp` — forward CONNECT_ACCEPT / CONNECT_REJECT
- Modify: `core/src/plugin.cpp` — init, tick, shutdown

- [ ] **Step 1: Plugin init + tick + shutdown in `core/src/plugin.cpp`**

Inside the `namespace kmp { ... }` forward-decl block at top:

```cpp
    void joiner_runtime_glue_init();
    void joiner_runtime_glue_shutdown();
    void joiner_runtime_glue_tick(float dt);
```

In `hooked_main_loop`, in the "Defer subsystem init" block (next to `server_browser_init`):

```cpp
        kmp::joiner_runtime_glue_init();
```

In `hooked_title_update`, next to `kmp::server_browser_tick(0.016f);`:

```cpp
    kmp::joiner_runtime_glue_tick(0.016f);
```

- [ ] **Step 2: player_sync forward CONNECT_ACCEPT / CONNECT_REJECT**

In `core/src/player_sync.cpp`, near existing extern decls at the top:

```cpp
extern void joiner_runtime_glue_on_connect_accept(uint32_t player_id);
extern void joiner_runtime_glue_on_connect_reject(const std::string& reason);
```

Find the `case PacketType::CONNECT_ACCEPT:` handler in the dispatch switch. At the very end of the case body (after `ui_on_connect_accept(pkt.player_id);`), add:

```cpp
            joiner_runtime_glue_on_connect_accept(pkt.player_id);
```

Find the `case PacketType::CONNECT_REJECT:` handler (likely also present). At the end of its body, add:

```cpp
            joiner_runtime_glue_on_connect_reject(pkt.reason);
```

(If CONNECT_REJECT isn't yet dispatched explicitly in player_sync.cpp, add a new case after CONNECT_ACCEPT:)

```cpp
    case PacketType::CONNECT_REJECT: {
        ConnectReject pkt;
        if (unpack(data, length, pkt)) {
            KMP_LOG(std::string("[KenshiMP] CONNECT_REJECT: ") + pkt.reason);
            joiner_runtime_glue_on_connect_reject(pkt.reason);
        }
        break;
    }
```

- [ ] **Step 3: server_browser — wire Join to joiner_runtime**

In `core/src/server_browser.cpp`, near the other extern decls at the top:

```cpp
namespace kmp {
    extern void joiner_runtime_glue_start(const ServerEntry& entry);
    extern void joiner_runtime_glue_cancel();
    extern int  joiner_runtime_glue_state_int();
    extern std::string joiner_runtime_glue_stage_label();
    extern std::string joiner_runtime_glue_progress_text();
    extern std::string joiner_runtime_glue_last_error();
}
```

Replace the body of `on_join` to start the runtime instead of just logging:

```cpp
static void on_join(MyGUI::Widget*) {
    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        if (e.id != s_selected_id) continue;
        char logbuf[256];
        _snprintf(logbuf, sizeof(logbuf),
            "[KenshiMP] Join: '%s' @ %s:%u",
            e.name.c_str(), e.address.c_str(), static_cast<unsigned>(e.port));
        KMP_LOG(logbuf);
        show_connecting_modal(e);
        joiner_runtime_glue_start(e);
        break;
    }
}
```

Replace the body of `on_connecting_cancel` to also cancel the runtime:

```cpp
static void on_connecting_cancel(MyGUI::Widget*) {
    KMP_LOG("[KenshiMP] Join cancelled by user");
    joiner_runtime_glue_cancel();
    hide_connecting_modal();
}
```

Replace `update_connecting_caption` to reflect real state:

```cpp
static void update_connecting_caption() {
    if (!s_connecting_visible || !s_connecting_label) return;
    int st = joiner_runtime_glue_state_int();
    std::string stage = joiner_runtime_glue_stage_label();
    std::string progress = joiner_runtime_glue_progress_text();

    ULONGLONG elapsed = GetTickCount64() - s_connecting_since_ms;
    int dots = (int)((elapsed / 500) % 3) + 1;
    const char* dot_str = (dots == 1) ? "." : (dots == 2) ? ".." : "...";

    std::string caption;
    // State::E values: 0=Idle 1=Downloading 2=Extracting 3=LoadTrigger
    // 4=LoadWait 5=EnetConnect 6=AwaitAccept 7=Done 8=Cancelled 9=Failed.
    if (st == 9) {  // Failed
        caption = "Error: " + joiner_runtime_glue_last_error();
    } else if (st == 7 || st == 8) {  // Done / Cancelled
        caption = "";
    } else {
        caption = stage + dot_str;
        if (!progress.empty()) caption += std::string("\n") + progress;
        caption += "\n\n" + s_connecting_server_line;
    }
    s_connecting_label->setCaption(caption);
}
```

Add a poll in `server_browser_tick` to auto-dismiss the modal on Done / Cancelled:

```cpp
void server_browser_tick(float /*dt*/) {
    if (!s_open) return;
    poll_ping_events();
    update_connecting_caption();
    if (s_connecting_visible && s_connecting_window) {
        try {
            MyGUI::LayerManager::getInstance().upLayerItem(s_connecting_window);
        } catch (...) { }
    }
    // Auto-close modal + browser on successful Done.
    int st = joiner_runtime_glue_state_int();
    if (s_connecting_visible && st == 7 /*Done*/) {
        hide_connecting_modal();
        server_browser_close();
    }
}
```

- [ ] **Step 4: Build + test**

```bash
make core 2>&1 | tail -5
make test 2>&1 | tail -8
```

Expected: clean build, all existing unit tests green.

- [ ] **Step 5: Commit**

```bash
git add core/src/server_browser.cpp core/src/player_sync.cpp core/src/plugin.cpp
git commit -m "feat(core): wire Join button to JoinerRuntime pipeline + auto-transition"
```

---

## Task 8: Manual smoke test

**Files:** none (documentation-only + runtime verification)

No automated test — Kenshi-in-the-loop. Use this as the final verification before merging to main.

- [ ] **Step 1: `make deploy`**

```bash
make deploy 2>&1 | tail -8
```

If Kenshi is running, close it first.

- [ ] **Step 2: Launch host session**

1. Launch Kenshi instance #1 (the host). Load a small save.
2. Click Multiplayer → Host (if you still have a working Host flow from A.2). Upload completes; `curl http://127.0.0.1:7778/snapshot -o /tmp/out.zip` returns a valid zip.

- [ ] **Step 3: Launch joiner**

1. Launch Kenshi instance #2 on the same PC (use a second Kenshi install or separate user profile).
2. Title screen → Multiplayer → Add server 127.0.0.1 → OK → ping succeeds.
3. Click Join (or double-click). Modal progresses:
   - `Downloading world.` `..` `...` with live MB count
   - `Extracting.` `..` `...`
   - `Loading world.` `..` `...`
   - `Connecting.` `..` `...`
   - Modal vanishes, Kenshi overlay vanishes, joiner is standing in the host's world.
4. Verify: joiner sees host's player character, host's NPCs, host's buildings.

- [ ] **Step 4: Failure flows**

Test password rejection:
1. Stop server. Edit `server_config.json` to add `"password": "hunter2"`. Restart server.
2. On joiner, edit the server entry to leave password empty. Click Join.
3. Modal progresses through download/extract/load, then at AwaitAccept: `Error: Wrong password`.
4. Click Close. Browser re-enabled.
5. Edit entry with correct password, Join again → succeeds.

Test cancel during download:
1. Start a fresh Join. During the Downloading phase, click Cancel.
2. Modal closes. Browser re-enabled. `/tmp/KMP_<hostid>.zip` and `<AppData>/Local/kenshi/save/KMP_<hostid>/` are not present or are cleaned up.

- [ ] **Step 5: Commit smoke-test completion note**

```bash
git commit --allow-empty -m "test: Plan A.4 manual smoke test passed (joiner pipeline end-to-end)"
```

---

## Final verification

```bash
make test
```

All 12 unit-test suites pass.

Manual tests from Task 8 succeed on a fresh Kenshi boot.

At this point Plan A.4 is complete — the A-series (host upload + joiner runtime + browser UI) is done. Next: Plans B (faction / character lifecycle), D (auto-save / session lifecycle), or E (richer runtime authority).

## Self-review

- **Spec coverage**: Every row in the failure table is tested. Happy-path covers every state transition. Cancel-per-stage verified via the download path; load/connect cancel are documented as "can't abort cleanly" and fall back to local-only state (matches spec).
- **Placeholders**: none — each step has concrete code.
- **Type consistency**: `JoinerRuntime::State`, `Deps`, and the state enum integer mappings (Done=7, Cancelled=8, Failed=9) are consistent between header, cpp, test file, and `update_connecting_caption`.
- **Scope**: focused on pipeline only. Per-player state, auto-save scheduling, richer authority all explicitly deferred.
