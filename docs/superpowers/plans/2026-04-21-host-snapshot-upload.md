# Host Snapshot Upload (Plan A.2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When the host clicks Host, the plugin saves the world to a dedicated `KMP_Session` slot, zips it on a background thread, and streams the zip to the server via `SNAPSHOT_UPLOAD_*` packets (already implemented server-side in Plan A.1). Status bar reflects each phase; failure is logged and terminal.

**Architecture:** Three new plugin-side files in `core/src/`:
- `save_trigger.{h,cpp}` thinly wraps `SaveFileSystem::getSingleton()->saveGame()` + `busy()`.
- `snapshot_zip.{h,cpp}` walks a directory and produces an in-RAM zip blob via miniz. Pure filesystem + miniz, no Kenshi.
- `snapshot_uploader.{h,cpp}` is the state machine driving `TRIGGER_SAVE → WAIT_SAVE → ZIP_START → ZIP_RUNNING → SEND_BEGIN → SEND_CHUNKS → SEND_END → AWAIT_ACK → IDLE` (or FAILED). All its external dependencies are function-object-injected so tests mock everything (no real threads, no real Kenshi).

**Tech Stack:** C++11 (plugin toolchain v100), miniz (vendored single-header + .c), `std::thread` + `std::atomic` for the zip worker, Windows `SHGetFolderPathW` for resolving `My Documents`, `FindFirstFileW` for directory walk. Tests under `tools/` build with v143 C++17 and use `KMP_CHECK`.

**Parent spec:** `docs/superpowers/specs/2026-04-21-host-snapshot-upload-design.md`.

**Depends on:** Plan A.1 (merged to main, provides server-side `SnapshotStore`, `SnapshotUploadSession`, `HttpSidecar`, packet types `SNAPSHOT_UPLOAD_BEGIN/CHUNK/END/ACK`).

---

## File Structure

**New files:**
- `deps/miniz/miniz.h` (vendored)
- `deps/miniz/miniz.c` (vendored)
- `deps/miniz/LICENSE` (vendored)
- `core/src/save_trigger.h` / `save_trigger.cpp` — Kenshi `SaveFileSystem` wrapper
- `core/src/snapshot_zip.h` / `snapshot_zip.cpp` — directory → in-RAM zip (miniz)
- `core/src/snapshot_uploader.h` / `snapshot_uploader.cpp` — state machine + DI
- `tools/test_snapshot_zip.cpp` — round-trip: zip a temp dir, unzip, compare
- `tools/test_snapshot_uploader.cpp` — mocked state-machine cases

**Modified:**
- `core/CMakeLists.txt` — register new sources + miniz + `deps/miniz` include path
- `core/src/player_sync.cpp` — dispatch `SNAPSHOT_UPLOAD_ACK` to uploader; tick uploader each frame
- `core/src/ui.cpp` — in `ui_on_connect_accept`, if host, call `snapshot_uploader::start("KMP_Session")`; in `update_status_text`, append progress text
- `tools/CMakeLists.txt` — two new test exes
- `Makefile` — add `test-snapshot-zip` and `test-snapshot-uploader` to `SNAPSHOT_UNIT_TESTS`

**Responsibility boundaries:**
- `save_trigger` knows Kenshi but not zip/network. Thin C-style namespace functions.
- `snapshot_zip` knows the filesystem + miniz, nothing else.
- `snapshot_uploader` owns the state machine; it knows *nothing* about its dependencies' internals — they're all injected lambdas.
- Plugin glue (`ui.cpp`, `player_sync.cpp`) is the only place where the three modules are wired together with real Kenshi / ENet / threads.

---

## Task 1: Vendor miniz

**Files:**
- Create: `deps/miniz/miniz.h`
- Create: `deps/miniz/miniz.c`
- Create: `deps/miniz/LICENSE`

miniz 3.0.2 is the current release. Its distribution is an "amalgamation" — two files, `miniz.c` + `miniz.h`. Public domain (MIT/unlicense-style).

- [ ] **Step 1: Create the directory**

```bash
mkdir -p deps/miniz
```

- [ ] **Step 2: Download the three files**

```bash
curl -L -o deps/miniz/miniz.h \
  https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz.h
curl -L -o deps/miniz/miniz.c \
  https://raw.githubusercontent.com/richgel999/miniz/3.0.2/miniz.c
curl -L -o deps/miniz/LICENSE \
  https://raw.githubusercontent.com/richgel999/miniz/3.0.2/LICENSE
```

Verify `miniz.h` is 150-250 KB and contains `MINIZ_VERSION`. Verify `miniz.c` exists (may be small: amalgamated includes). If `miniz.c` is < 5 KB, the distribution layout changed — fall back to the amalgamation in `amalgamation/` branch:

```bash
# Fallback if miniz.c is too small
curl -L -o deps/miniz/miniz.c \
  https://raw.githubusercontent.com/richgel999/miniz/3.0.2/amalgamation/miniz.c
curl -L -o deps/miniz/miniz.h \
  https://raw.githubusercontent.com/richgel999/miniz/3.0.2/amalgamation/miniz.h
```

Verify `miniz.h` now compiles as C standalone (no weird includes to missing files) by searching for `#include` lines that aren't stdlib/system headers:

```bash
grep -E '^#include "' deps/miniz/miniz.h deps/miniz/miniz.c | grep -v 'miniz'
```

Expected: no output (miniz is self-contained). If any non-miniz `#include "..."` appears, the amalgamation is broken — check the release assets page at https://github.com/richgel999/miniz/releases/tag/3.0.2 and pick the single-file amalgamation zip instead.

- [ ] **Step 3: Commit**

```bash
git add deps/miniz
git commit -m "deps: vendor miniz 3.0.2 for in-memory zip creation"
```

---

## Task 2: snapshot_zip module

**Files:**
- Create: `core/src/snapshot_zip.h`
- Create: `core/src/snapshot_zip.cpp`
- Create: `tools/test_snapshot_zip.cpp`
- Modify: `core/CMakeLists.txt` (add source + miniz)
- Modify: `tools/CMakeLists.txt` (add test target)
- Modify: `Makefile` (add to `SNAPSHOT_UNIT_TESTS`)

Directory-to-zip-blob via miniz. Single public entry point `kmp::zip_directory(const std::string& abs_path)` returns `std::vector<uint8_t>`. Returns empty on any failure.

- [ ] **Step 1: Write the failing test**

Create `tools/test_snapshot_zip.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "snapshot_zip.h"
#include "miniz.h"

namespace fs = std::filesystem;

static fs::path make_tempdir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("kmp_zip_test_" + std::string(name) + "_" + std::to_string(std::time(nullptr)));
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

static void test_roundtrip_flat_and_nested() {
    fs::path src = make_tempdir("src");
    write_file(src / "a.txt",           "hello world");
    write_file(src / "sub" / "b.bin",   std::string("\x00\x01\x02\x03\x04", 5));
    write_file(src / "sub" / "nest" / "c.txt", "deeply nested");

    std::vector<uint8_t> blob = kmp::zip_directory(src.string());
    KMP_CHECK(!blob.empty());

    // Round-trip: open the blob with miniz and extract to a second tempdir.
    fs::path dst = make_tempdir("dst");
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    KMP_CHECK(mz_zip_reader_init_mem(&zip, blob.data(), blob.size(), 0));

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        KMP_CHECK(mz_zip_reader_file_stat(&zip, i, &stat));
        if (stat.m_is_directory) continue;
        fs::path out = dst / stat.m_filename;
        fs::create_directories(out.parent_path());
        KMP_CHECK(mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0));
    }
    mz_zip_reader_end(&zip);

    KMP_CHECK(read_file(dst / "a.txt") == "hello world");
    KMP_CHECK(read_file(dst / "sub" / "b.bin") == std::string("\x00\x01\x02\x03\x04", 5));
    KMP_CHECK(read_file(dst / "sub" / "nest" / "c.txt") == "deeply nested");

    fs::remove_all(src);
    fs::remove_all(dst);
    printf("test_roundtrip_flat_and_nested OK\n");
}

static void test_missing_dir_returns_empty() {
    std::vector<uint8_t> blob = kmp::zip_directory("C:/this/path/does/not/exist/kmp_xyz");
    KMP_CHECK(blob.empty());
    printf("test_missing_dir_returns_empty OK\n");
}

int main() {
    test_roundtrip_flat_and_nested();
    test_missing_dir_returns_empty();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt` (match pattern of existing `test-snapshot-store`):

```cmake
add_executable(test-snapshot-zip test_snapshot_zip.cpp
    ${CMAKE_SOURCE_DIR}/core/src/snapshot_zip.cpp
    ${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c)
target_include_directories(test-snapshot-zip PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/deps/miniz)
target_link_libraries(test-snapshot-zip PRIVATE kenshi-mp-common)
target_compile_features(test-snapshot-zip PRIVATE cxx_std_17)
```

Add to `Makefile` — extend `SNAPSHOT_UNIT_TESTS`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip
```

- [ ] **Step 2: Verify it fails to build**

```bash
make test 2>&1 | tail -15
```

Expected: build error — `snapshot_zip.h` not found.

- [ ] **Step 3: Create snapshot_zip.h**

```cpp
// snapshot_zip.h — Walk a directory and produce an in-RAM zip blob via miniz.
//
// Pure filesystem + compression. Does NOT include any Kenshi header — safe
// to run from a background thread alongside Kenshi's render/update threads.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kmp {

/// Recursively zip every regular file under `abs_path` into an in-memory
/// zip archive. Entry names are stored as paths relative to `abs_path`
/// using forward slashes.
///
/// Returns an empty vector on any failure (missing dir, miniz error, etc.).
/// Logs nothing — caller decides what to report.
std::vector<uint8_t> zip_directory(const std::string& abs_path);

} // namespace kmp
```

- [ ] **Step 4: Create snapshot_zip.cpp**

```cpp
#include "snapshot_zip.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "miniz.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace kmp {

namespace {

// Convert the Windows wide "\\?\" path to forward-slash UTF-8, relative to
// `root_w`. Used for archive-entry names so they're portable across OSes.
static std::string make_archive_name(const std::wstring& full_w,
                                     const std::wstring& root_w) {
    std::wstring rel_w;
    if (full_w.size() > root_w.size() &&
        full_w.compare(0, root_w.size(), root_w) == 0) {
        rel_w = full_w.substr(root_w.size());
    } else {
        rel_w = full_w;
    }
    while (!rel_w.empty() && (rel_w[0] == L'\\' || rel_w[0] == L'/')) {
        rel_w.erase(0, 1);
    }
    // Convert to UTF-8.
    int needed = WideCharToMultiByte(CP_UTF8, 0, rel_w.c_str(),
                                     static_cast<int>(rel_w.size()),
                                     nullptr, 0, nullptr, nullptr);
    std::string rel(needed, '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, rel_w.c_str(),
                            static_cast<int>(rel_w.size()),
                            &rel[0], needed, nullptr, nullptr);
    }
    // Normalise backslashes → forward slashes.
    for (auto& c : rel) if (c == '\\') c = '/';
    return rel;
}

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                     static_cast<int>(s.size()),
                                     nullptr, 0);
    std::wstring w(needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], needed);
    return w;
}

// Walk recursively. Returns false on any filesystem error; callers treat
// that as a zip failure.
static bool walk(const std::wstring& root_w,
                 const std::wstring& cur_w,
                 mz_zip_archive& zip) {
    std::wstring pattern = cur_w + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        std::wstring child = cur_w + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!walk(root_w, child, zip)) { ok = false; break; }
            continue;
        }

        // Read the file into memory and add it.
        std::ifstream f;
        f.open(child.c_str(), std::ios::binary);
        if (!f.is_open()) { ok = false; break; }
        std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());

        std::string archive_name = make_archive_name(child, root_w);
        if (!mz_zip_writer_add_mem(&zip, archive_name.c_str(),
                                   buf.data(), buf.size(),
                                   MZ_DEFAULT_LEVEL)) {
            ok = false;
            break;
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return ok;
}

} // namespace

std::vector<uint8_t> zip_directory(const std::string& abs_path) {
    std::wstring root_w = utf8_to_wide(abs_path);

    // Strip any trailing separator so FindFirstFileW doesn't choke.
    while (!root_w.empty() &&
           (root_w.back() == L'\\' || root_w.back() == L'/')) {
        root_w.pop_back();
    }

    DWORD attrs = GetFileAttributesW(root_w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return {};
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 16 * 1024)) {
        return {};
    }

    bool ok = walk(root_w, root_w, zip);

    if (!ok) {
        mz_zip_writer_end(&zip);
        return {};
    }

    void* out_ptr = nullptr;
    size_t out_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &out_ptr, &out_size)) {
        mz_zip_writer_end(&zip);
        return {};
    }

    std::vector<uint8_t> result(out_size);
    std::memcpy(result.data(), out_ptr, out_size);
    mz_free(out_ptr);
    mz_zip_writer_end(&zip);
    return result;
}

} // namespace kmp
```

- [ ] **Step 5: Register sources in core CMakeLists**

Edit `core/CMakeLists.txt`. After the existing `add_library(KenshiMP SHARED ...)` list, add `src/snapshot_zip.cpp` and `${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c` to the source list. Also add a new include directory line:

```cmake
add_library(KenshiMP SHARED
    src/plugin.cpp
    src/client.cpp
    src/game_hooks.cpp
    src/player_sync.cpp
    src/npc_manager.cpp
    src/host_sync.cpp
    src/building_sync.cpp
    src/building_manager.cpp
    src/admin_panel.cpp
    src/ui.cpp
    src/client_identity.cpp
    src/snapshot_zip.cpp
    ${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c
)
```

And below the existing target properties / include directives (look for `target_include_directories` — if absent, add one):

```cmake
target_include_directories(KenshiMP PRIVATE ${CMAKE_SOURCE_DIR}/deps/miniz)
```

- [ ] **Step 6: Run tests**

```bash
make test 2>&1 | tail -15
```

Expected output (amongst the other test suites):
```
--- test-snapshot-zip ---
test_roundtrip_flat_and_nested OK
test_missing_dir_returns_empty OK
ALL PASS
```

If miniz.c fails to compile under v143 (tool-side build), the tools CMakeLists may need `target_compile_definitions(... MINIZ_NO_STDIO=0)` — but miniz compiles cleanly with defaults, so expect no adjustments.

- [ ] **Step 7: Commit**

```bash
git add core/src/snapshot_zip.h core/src/snapshot_zip.cpp \
        core/CMakeLists.txt \
        tools/test_snapshot_zip.cpp tools/CMakeLists.txt \
        Makefile
git commit -m "feat(core): zip_directory() via miniz for snapshot producer"
```

---

## Task 3: save_trigger module

**Files:**
- Create: `core/src/save_trigger.h`
- Create: `core/src/save_trigger.cpp`
- Modify: `core/CMakeLists.txt` (add source)

Thin wrapper around `SaveFileSystem::getSingleton()->saveGame()` and `busy()`. Also resolves the absolute slot path via `SHGetFolderPathW`.

No unit tests: every call goes into Kenshi, can't be isolated. The uploader tests mock this module entirely.

- [ ] **Step 1: Create save_trigger.h**

```cpp
// save_trigger.h — Thin wrapper around Kenshi's SaveFileSystem.
//
// Kenshi's save is asynchronous: saveGame() returns immediately and the
// save happens on Kenshi's own worker thread. Callers poll is_saving()
// until it returns false to know the save is complete.
#pragma once

#include <string>

namespace kmp {

/// Kick off a save of the current world state to the named slot.
/// Blocks only briefly (enqueues onto Kenshi's save thread).
/// Returns false if SaveFileSystem is unreachable.
bool save_trigger_start(const std::string& slot_name);

/// True while Kenshi's save is running. Poll this after save_trigger_start
/// until it returns false.
bool save_trigger_is_busy();

/// Resolve the on-disk path for a named slot:
///   <Documents>/My Games/Kenshi/save/<slot_name>/
/// Returns empty string if Documents resolution fails.
std::string save_trigger_resolve_slot_path(const std::string& slot_name);

} // namespace kmp
```

- [ ] **Step 2: Create save_trigger.cpp**

```cpp
#include "save_trigger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <kenshi/SaveFileSystem.h>

#include "kmp_log.h"

namespace kmp {

namespace {

static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                     static_cast<int>(w.size()),
                                     nullptr, 0, nullptr, nullptr);
    std::string s(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], needed, nullptr, nullptr);
    return s;
}

static std::string documents_path() {
    wchar_t buf[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr,
                                    SHGFP_TYPE_CURRENT, buf))) {
        return {};
    }
    return wide_to_utf8(buf);
}

} // namespace

bool save_trigger_start(const std::string& slot_name) {
    SaveFileSystem* sfs = SaveFileSystem::getSingleton();
    if (!sfs) {
        KMP_LOG("[KenshiMP] save_trigger: SaveFileSystem singleton is null");
        return false;
    }
    std::string path = save_trigger_resolve_slot_path(slot_name);
    if (path.empty()) {
        KMP_LOG("[KenshiMP] save_trigger: could not resolve Documents path");
        return false;
    }
    bool ok = sfs->saveGame(path);
    KMP_LOG(std::string("[KenshiMP] save_trigger: saveGame('") + path
            + "') returned " + (ok ? "true" : "false"));
    return ok;
}

bool save_trigger_is_busy() {
    SaveFileSystem* sfs = SaveFileSystem::getSingleton();
    if (!sfs) return false;
    return sfs->busy();
}

std::string save_trigger_resolve_slot_path(const std::string& slot_name) {
    std::string docs = documents_path();
    if (docs.empty()) return {};
    std::string path = docs + "\\My Games\\Kenshi\\save\\" + slot_name;
    return path;
}

} // namespace kmp
```

- [ ] **Step 3: Register in core CMakeLists**

In `core/CMakeLists.txt`, add `src/save_trigger.cpp` to the `add_library(KenshiMP SHARED ...)` source list.

- [ ] **Step 4: Build verify**

```bash
make core 2>&1 | tail -10
```

Expected: clean build. No test target for this module — it's verified via the uploader integration.

- [ ] **Step 5: Commit**

```bash
git add core/src/save_trigger.h core/src/save_trigger.cpp core/CMakeLists.txt
git commit -m "feat(core): save_trigger wraps SaveFileSystem + Documents path resolver"
```

---

## Task 4: snapshot_uploader state machine + unit tests

**Files:**
- Create: `core/src/snapshot_uploader.h`
- Create: `core/src/snapshot_uploader.cpp`
- Create: `tools/test_snapshot_uploader.cpp`
- Modify: `core/CMakeLists.txt` (add source)
- Modify: `tools/CMakeLists.txt` (add test target)
- Modify: `Makefile` (add to `SNAPSHOT_UNIT_TESTS`)

The core of the plan. State machine with dependency-injected external calls so tests run without threads, without Kenshi, and without ENet.

- [ ] **Step 1: Write the failing test**

Create `tools/test_snapshot_uploader.cpp`:

```cpp
#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "packets.h"
#include "serialization.h"
#include "snapshot_uploader.h"

using namespace kmp;

namespace {

// Recorder for sent bytes. After a full upload we should have exactly one
// BEGIN, N CHUNKs covering the whole blob, and one END.
struct Recorder {
    std::vector<std::vector<uint8_t>> sends;
};

static SnapshotUploader::Deps make_happy_deps(
        Recorder* rec,
        bool* save_busy,
        bool* zip_started,
        std::vector<uint8_t>* zip_out,
        float* clock) {
    SnapshotUploader::Deps d;
    d.trigger_save = [save_busy](const std::string& slot) -> bool {
        (void)slot;
        *save_busy = true;
        return true;
    };
    d.is_save_busy = [save_busy]() -> bool { return *save_busy; };
    d.resolve_slot_path = [](const std::string& slot) -> std::string {
        return "C:/tmp/" + slot;
    };
    d.start_zip = [zip_started](const std::string& path) {
        (void)path;
        *zip_started = true;
    };
    d.poll_zip = [zip_started, zip_out](std::vector<uint8_t>& out) -> bool {
        if (!*zip_started) return false;
        if (zip_out->empty()) return false;
        out = *zip_out;
        return true;
    };
    d.send_reliable = [rec](const uint8_t* data, size_t len) -> bool {
        rec->sends.emplace_back(data, data + len);
        return true;
    };
    d.now_seconds = [clock]() -> float { return *clock; };
    return d;
}

static void test_happy_path() {
    Recorder rec;
    bool save_busy = false;
    bool zip_started = false;
    std::vector<uint8_t> zip_blob;
    float clock = 0.0f;

    SnapshotUploader up(make_happy_deps(&rec, &save_busy, &zip_started, &zip_blob, &clock));
    KMP_CHECK(up.state() == SnapshotUploader::State::IDLE);

    up.start("KMP_Session");
    KMP_CHECK(up.state() == SnapshotUploader::State::WAIT_SAVE);

    // Kenshi hasn't finished saving yet.
    up.tick(0.016f); clock += 0.016f;
    KMP_CHECK(up.state() == SnapshotUploader::State::WAIT_SAVE);

    // Save completes.
    save_busy = false;
    up.tick(0.016f); clock += 0.016f;
    KMP_CHECK(up.state() == SnapshotUploader::State::ZIP_RUNNING);
    KMP_CHECK(zip_started);

    // Zip finishes. Fill the blob with 200 KB of known bytes so we get
    // multiple chunks.
    zip_blob.resize(200 * 1024);
    for (size_t i = 0; i < zip_blob.size(); ++i) {
        zip_blob[i] = static_cast<uint8_t>(i & 0xFF);
    }
    up.tick(0.016f); clock += 0.016f;
    // After zip completion, BEGIN is sent and we're now sending chunks.
    KMP_CHECK(up.state() == SnapshotUploader::State::SEND_CHUNKS);
    KMP_CHECK(rec.sends.size() == 1);  // BEGIN
    {
        PacketHeader h;
        KMP_CHECK(peek_header(rec.sends[0].data(), rec.sends[0].size(), h));
        KMP_CHECK(h.type == PacketType::SNAPSHOT_UPLOAD_BEGIN);
    }

    // Drive chunks through. Give each tick enough time to send one chunk.
    int safety = 1000;
    while (up.state() == SnapshotUploader::State::SEND_CHUNKS && safety-- > 0) {
        up.tick(0.016f); clock += 0.016f;
    }
    KMP_CHECK(safety > 0);
    KMP_CHECK(up.state() == SnapshotUploader::State::AWAIT_ACK);

    // The last send must be an END.
    {
        PacketHeader h;
        auto& last = rec.sends.back();
        KMP_CHECK(peek_header(last.data(), last.size(), h));
        KMP_CHECK(h.type == PacketType::SNAPSHOT_UPLOAD_END);
    }
    // Everything between the first (BEGIN) and last (END) is a CHUNK and
    // their payload covers exactly the whole blob, in order.
    size_t total = 0;
    for (size_t i = 1; i + 1 < rec.sends.size(); ++i) {
        PacketHeader h;
        KMP_CHECK(peek_header(rec.sends[i].data(), rec.sends[i].size(), h));
        KMP_CHECK(h.type == PacketType::SNAPSHOT_UPLOAD_CHUNK);
        SnapshotUploadChunk hdr;
        const uint8_t* tail = nullptr;
        size_t tail_len = 0;
        KMP_CHECK(unpack_with_tail(rec.sends[i].data(), rec.sends[i].size(),
                                   hdr, tail, tail_len));
        KMP_CHECK(hdr.offset == total);
        KMP_CHECK(hdr.length <= tail_len);
        KMP_CHECK(std::memcmp(tail, zip_blob.data() + total, hdr.length) == 0);
        total += hdr.length;
    }
    KMP_CHECK(total == zip_blob.size());

    // Inject ACK.
    SnapshotUploadAck ack;
    ack.upload_id = 0;  // will match whatever the uploader chose
    // We need to dig the upload_id out of the BEGIN we recorded.
    {
        SnapshotUploadBegin begin;
        KMP_CHECK(unpack(rec.sends[0].data(), rec.sends[0].size(), begin));
        ack.upload_id = begin.upload_id;
    }
    ack.accepted   = 1;
    ack.error_code = 0;
    up.on_ack(ack);
    KMP_CHECK(up.state() == SnapshotUploader::State::IDLE);

    printf("test_happy_path OK\n");
}

static void test_save_trigger_failure() {
    Recorder rec;
    float clock = 0.0f;
    SnapshotUploader::Deps d;
    d.trigger_save       = [](const std::string&) { return false; };
    d.is_save_busy       = []() { return false; };
    d.resolve_slot_path  = [](const std::string& s) { return "C:/" + s; };
    d.start_zip          = [](const std::string&) {};
    d.poll_zip           = [](std::vector<uint8_t>&) { return false; };
    d.send_reliable      = [&](const uint8_t*, size_t) { return true; };
    d.now_seconds        = [&]() { return clock; };

    SnapshotUploader up(d);
    up.start("KMP_Session");
    // start() itself called trigger_save, which returned false → FAILED.
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("save") != std::string::npos);

    printf("test_save_trigger_failure OK\n");
}

static void test_save_wait_timeout() {
    Recorder rec;
    bool save_busy = true;
    float clock = 0.0f;
    SnapshotUploader::Deps d;
    d.trigger_save       = [&](const std::string&) { save_busy = true; return true; };
    d.is_save_busy       = [&]() { return save_busy; };
    d.resolve_slot_path  = [](const std::string& s) { return "C:/" + s; };
    d.start_zip          = [](const std::string&) {};
    d.poll_zip           = [](std::vector<uint8_t>&) { return false; };
    d.send_reliable      = [&](const uint8_t*, size_t) { return true; };
    d.now_seconds        = [&]() { return clock; };

    SnapshotUploader up(d);
    up.start("KMP_Session");
    KMP_CHECK(up.state() == SnapshotUploader::State::WAIT_SAVE);

    // Simulate 61 seconds of polling while save_busy stays true.
    for (int i = 0; i < 61; ++i) {
        clock += 1.0f;
        up.tick(1.0f);
    }
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("timed out") != std::string::npos);

    printf("test_save_wait_timeout OK\n");
}

static void test_zip_empty_blob_fails() {
    Recorder rec;
    bool save_busy = false;
    bool zip_started = false;
    std::vector<uint8_t> empty_blob;  // stays empty
    float clock = 0.0f;

    SnapshotUploader::Deps d;
    d.trigger_save       = [&](const std::string&) { save_busy = false; return true; };
    d.is_save_busy       = [&]() { return save_busy; };
    d.resolve_slot_path  = [](const std::string& s) { return "C:/" + s; };
    d.start_zip          = [&](const std::string&) { zip_started = true; };
    // poll_zip signals "done" by returning true with an empty vector.
    d.poll_zip           = [&](std::vector<uint8_t>& out) -> bool {
        if (!zip_started) return false;
        out.clear();  // empty — simulates zip failure
        return true;
    };
    d.send_reliable      = [&](const uint8_t*, size_t) { return true; };
    d.now_seconds        = [&]() { return clock; };

    SnapshotUploader up(d);
    up.start("KMP_Session");
    up.tick(0.016f);  // transition into ZIP_RUNNING
    up.tick(0.016f);  // poll_zip returns true with empty blob
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("packaging") != std::string::npos);

    printf("test_zip_empty_blob_fails OK\n");
}

static void test_ack_rejected() {
    // Run happy path up through AWAIT_ACK, then inject a rejected ACK.
    Recorder rec;
    bool save_busy = false;
    bool zip_started = false;
    std::vector<uint8_t> blob(1024, 0xAB);
    float clock = 0.0f;

    SnapshotUploader up(make_happy_deps(&rec, &save_busy, &zip_started, &blob, &clock));
    up.start("KMP_Session");
    while (up.state() != SnapshotUploader::State::AWAIT_ACK) {
        up.tick(0.016f); clock += 0.016f;
    }

    SnapshotUploadBegin begin;
    KMP_CHECK(unpack(rec.sends[0].data(), rec.sends[0].size(), begin));

    SnapshotUploadAck ack;
    ack.upload_id  = begin.upload_id;
    ack.accepted   = 0;
    ack.error_code = 1;  // ShaMismatch
    up.on_ack(ack);
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("rejected") != std::string::npos);

    printf("test_ack_rejected OK\n");
}

static void test_ack_timeout() {
    Recorder rec;
    bool save_busy = false;
    bool zip_started = false;
    std::vector<uint8_t> blob(1024, 0xCD);
    float clock = 0.0f;

    SnapshotUploader up(make_happy_deps(&rec, &save_busy, &zip_started, &blob, &clock));
    up.start("KMP_Session");
    while (up.state() != SnapshotUploader::State::AWAIT_ACK) {
        up.tick(0.016f); clock += 0.016f;
    }

    for (int i = 0; i < 31; ++i) {
        clock += 1.0f;
        up.tick(1.0f);
    }
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("ACK") != std::string::npos
           || up.last_error().find("confirm") != std::string::npos);

    printf("test_ack_timeout OK\n");
}

} // namespace

int main() {
    test_happy_path();
    test_save_trigger_failure();
    test_save_wait_timeout();
    test_zip_empty_blob_fails();
    test_ack_rejected();
    test_ack_timeout();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-snapshot-uploader test_snapshot_uploader.cpp
    ${CMAKE_SOURCE_DIR}/core/src/snapshot_uploader.cpp)
target_include_directories(test-snapshot-uploader PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(test-snapshot-uploader PRIVATE kenshi-mp-common)
target_compile_features(test-snapshot-uploader PRIVATE cxx_std_17)
```

And extend the Makefile's `SNAPSHOT_UNIT_TESTS`:

```makefile
SNAPSHOT_UNIT_TESTS := test-snapshot-packets test-snapshot-store \
                       test-snapshot-upload test-http-sidecar \
                       test-snapshot-zip test-snapshot-uploader
```

- [ ] **Step 2: Verify build fails**

```bash
make test 2>&1 | tail -15
```

Expected: `snapshot_uploader.h` not found.

- [ ] **Step 3: Create snapshot_uploader.h**

```cpp
// snapshot_uploader.h — State machine that drives a save → zip → upload
// sequence. All external dependencies are injected as std::function so
// tests run synchronously without Kenshi, threads, or ENet.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kmp {

struct SnapshotUploadAck;

class SnapshotUploader {
public:
    enum class State {
        IDLE,
        WAIT_SAVE,      // Kenshi is saving
        ZIP_RUNNING,    // background thread is zipping
        SEND_CHUNKS,    // chunks streaming to server
        AWAIT_ACK,      // END sent, waiting for server to confirm
        FAILED,
    };

    struct Deps {
        /// Kick off Kenshi's save. Returns false on immediate failure.
        std::function<bool(const std::string& slot)>              trigger_save;
        /// True while Kenshi is still writing the save.
        std::function<bool()>                                     is_save_busy;
        /// Absolute filesystem path for a named save slot.
        std::function<std::string(const std::string& slot)>       resolve_slot_path;
        /// Kick off an async zip of the given directory. Non-blocking.
        std::function<void(const std::string& abs_path)>          start_zip;
        /// Returns true (and fills out) once the async zip is done. False
        /// while still running. An empty out-blob means the zip failed.
        std::function<bool(std::vector<uint8_t>& out)>            poll_zip;
        /// Send bytes on the ENet reliable channel.
        std::function<bool(const uint8_t* data, size_t len)>      send_reliable;
        /// Monotonic seconds for timeouts.
        std::function<float()>                                    now_seconds;
    };

    explicit SnapshotUploader(Deps deps);

    /// Begin a new upload of `slot_name`. Resets state; if called while an
    /// upload is in progress, the in-progress one is discarded.
    void start(const std::string& slot_name);

    /// Drive the state machine. Called every frame from player_sync_tick.
    void tick(float dt);

    /// Called from the client-packet receive path when an ACK arrives.
    void on_ack(const SnapshotUploadAck& ack);

    State              state() const;
    const std::string& last_error() const;
    /// One-line status suitable for displaying in the game UI.
    std::string        progress_text() const;

private:
    Deps m_deps;
    State m_state;
    std::string m_slot_name;
    std::string m_slot_path;

    // WAIT_SAVE / AWAIT_ACK timing
    float m_enter_wait_t;
    float m_enter_ack_t;

    // Zip output (populated when poll_zip returns true)
    std::vector<uint8_t> m_blob;

    // Upload progress
    uint32_t m_upload_id;
    uint64_t m_offset;         // bytes of blob sent so far

    // Error messaging
    std::string m_error;

    void go_failed(const std::string& err);
    void start_sending_begin();      // transitions to SEND_CHUNKS
    void send_next_chunks();         // sends up to CHUNKS_PER_TICK chunks
    void send_end();                 // transitions to AWAIT_ACK
};

} // namespace kmp
```

- [ ] **Step 4: Create snapshot_uploader.cpp**

```cpp
#include "snapshot_uploader.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "packets.h"
#include "serialization.h"
#include "picosha2.h"

namespace kmp {

namespace {

constexpr float    kSaveTimeoutSec  = 60.0f;
constexpr float    kAckTimeoutSec   = 30.0f;
constexpr uint16_t kChunkSize       = 60 * 1024;   // 60 KB
constexpr int      kChunksPerTick   = 2;
constexpr uint64_t kMaxBlobBytes    = 512ull * 1024 * 1024;  // match server

static uint32_t rand32() {
    // std::rand is plenty for a non-cryptographic upload id.
    return (static_cast<uint32_t>(std::rand()) << 16) ^ static_cast<uint32_t>(std::rand());
}

} // namespace

SnapshotUploader::SnapshotUploader(Deps deps)
    : m_deps(std::move(deps))
    , m_state(State::IDLE)
    , m_enter_wait_t(0.0f)
    , m_enter_ack_t(0.0f)
    , m_upload_id(0)
    , m_offset(0) {}

void SnapshotUploader::start(const std::string& slot_name) {
    m_slot_name = slot_name;
    m_slot_path = m_deps.resolve_slot_path(slot_name);
    m_blob.clear();
    m_offset = 0;
    m_upload_id = rand32();
    m_error.clear();

    if (m_slot_path.empty()) {
        go_failed("save folder not found");
        return;
    }

    if (!m_deps.trigger_save(m_slot_name)) {
        go_failed("save failed (Kenshi refused)");
        return;
    }

    m_state = State::WAIT_SAVE;
    m_enter_wait_t = m_deps.now_seconds();
}

void SnapshotUploader::tick(float /*dt*/) {
    switch (m_state) {
    case State::IDLE:
    case State::FAILED:
        return;

    case State::WAIT_SAVE: {
        if (m_deps.is_save_busy()) {
            if (m_deps.now_seconds() - m_enter_wait_t > kSaveTimeoutSec) {
                go_failed("save timed out (60s)");
            }
            return;
        }
        // Save complete. Kick off zip on a worker.
        m_deps.start_zip(m_slot_path);
        m_state = State::ZIP_RUNNING;
        return;
    }

    case State::ZIP_RUNNING: {
        std::vector<uint8_t> out;
        if (!m_deps.poll_zip(out)) return;
        if (out.empty()) { go_failed("packaging failed"); return; }
        if (out.size() > kMaxBlobBytes) {
            go_failed("save too large (" + std::to_string(out.size() / (1024 * 1024))
                      + " MB > 512 MB)");
            return;
        }
        m_blob = std::move(out);
        start_sending_begin();
        return;
    }

    case State::SEND_CHUNKS: {
        send_next_chunks();
        return;
    }

    case State::AWAIT_ACK: {
        if (m_deps.now_seconds() - m_enter_ack_t > kAckTimeoutSec) {
            go_failed("server didn't confirm upload");
        }
        return;
    }
    }
}

void SnapshotUploader::on_ack(const SnapshotUploadAck& ack) {
    if (m_state != State::AWAIT_ACK) return;
    if (ack.upload_id != m_upload_id) return;

    if (ack.accepted == 1) {
        m_state = State::IDLE;
        return;
    }

    std::string msg = "server rejected upload (code=";
    msg += std::to_string(static_cast<int>(ack.error_code));
    msg += ")";
    go_failed(msg);
}

SnapshotUploader::State SnapshotUploader::state() const { return m_state; }
const std::string& SnapshotUploader::last_error() const { return m_error; }

std::string SnapshotUploader::progress_text() const {
    switch (m_state) {
    case State::IDLE:         return "";
    case State::WAIT_SAVE:    return "Hosting: saving world...";
    case State::ZIP_RUNNING:  return "Hosting: packaging world...";
    case State::SEND_CHUNKS: {
        float mb_sent  = static_cast<float>(m_offset) / (1024.0f * 1024.0f);
        float mb_total = static_cast<float>(m_blob.size()) / (1024.0f * 1024.0f);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "Hosting: uploading %.1f / %.1f MB", mb_sent, mb_total);
        return buf;
    }
    case State::AWAIT_ACK:    return "Hosting: finalising...";
    case State::FAILED:       return "Hosting: upload failed (" + m_error + ")";
    }
    return "";
}

void SnapshotUploader::go_failed(const std::string& err) {
    m_error = err;
    m_state = State::FAILED;
}

void SnapshotUploader::start_sending_begin() {
    if (m_blob.empty()) { go_failed("packaging failed"); return; }

    SnapshotUploadBegin begin;
    begin.upload_id  = m_upload_id;
    begin.rev        = 1;
    begin.total_size = m_blob.size();
    picosha2::hash256(m_blob.begin(), m_blob.end(),
                      begin.sha256, begin.sha256 + 32);

    auto buf = pack(begin);
    if (!m_deps.send_reliable(buf.data(), buf.size())) {
        go_failed("send failed");
        return;
    }

    m_offset = 0;
    m_state  = State::SEND_CHUNKS;
}

void SnapshotUploader::send_next_chunks() {
    for (int i = 0; i < kChunksPerTick; ++i) {
        if (m_offset >= m_blob.size()) { send_end(); return; }

        uint16_t len = static_cast<uint16_t>(
            std::min<uint64_t>(kChunkSize, m_blob.size() - m_offset));

        SnapshotUploadChunk hdr;
        hdr.upload_id = m_upload_id;
        hdr.offset    = static_cast<uint32_t>(m_offset);
        hdr.length    = len;
        auto buf = pack_with_tail(hdr, m_blob.data() + m_offset, len);
        if (!m_deps.send_reliable(buf.data(), buf.size())) {
            go_failed("send failed");
            return;
        }
        m_offset += len;
    }
}

void SnapshotUploader::send_end() {
    SnapshotUploadEnd end;
    end.upload_id = m_upload_id;
    auto buf = pack(end);
    if (!m_deps.send_reliable(buf.data(), buf.size())) {
        go_failed("send failed");
        return;
    }
    m_state = State::AWAIT_ACK;
    m_enter_ack_t = m_deps.now_seconds();
}

} // namespace kmp
```

Note: `snapshot_uploader.cpp` pulls in `picosha2.h` (vendored in Plan A.1) for the sha256 computation of the zip blob. Add the include path to `core/CMakeLists.txt`:

```cmake
target_include_directories(KenshiMP PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/miniz
    ${CMAKE_SOURCE_DIR}/deps/picosha2
)
```

(Adjust the existing `target_include_directories` line from Task 2 to include both; don't create a second call.)

Same for `tools/CMakeLists.txt` — the `test-snapshot-uploader` target needs `${CMAKE_SOURCE_DIR}/deps/picosha2` on its include path. Update the Task 4 Step 1 CMakeLists addition to:

```cmake
add_executable(test-snapshot-uploader test_snapshot_uploader.cpp
    ${CMAKE_SOURCE_DIR}/core/src/snapshot_uploader.cpp)
target_include_directories(test-snapshot-uploader PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/deps/picosha2)
target_link_libraries(test-snapshot-uploader PRIVATE kenshi-mp-common)
target_compile_features(test-snapshot-uploader PRIVATE cxx_std_17)
```

- [ ] **Step 5: Register source in core CMakeLists**

Add `src/snapshot_uploader.cpp` to `core/CMakeLists.txt`'s `add_library` source list. Ensure the `target_include_directories` call includes both `deps/miniz` and `deps/picosha2`.

- [ ] **Step 6: Build + run tests**

```bash
make test 2>&1 | tail -25
```

Expected:
```
--- test-snapshot-uploader ---
test_happy_path OK
test_save_trigger_failure OK
test_save_wait_timeout OK
test_zip_empty_blob_fails OK
test_ack_rejected OK
test_ack_timeout OK
ALL PASS
```

If `test_happy_path` fails with a chunk-coverage assertion: verify `pack_with_tail` byte layout — the tail bytes should follow the struct exactly, length field matching.

- [ ] **Step 7: Commit**

```bash
git add core/src/snapshot_uploader.h core/src/snapshot_uploader.cpp \
        core/CMakeLists.txt \
        tools/test_snapshot_uploader.cpp tools/CMakeLists.txt \
        Makefile
git commit -m "feat(core): snapshot_uploader state machine with dependency-injected tests"
```

---

## Task 5: Wire uploader into plugin

**Files:**
- Modify: `core/src/ui.cpp` — `ui_on_connect_accept` triggers upload if host; `update_status_text` shows progress
- Modify: `core/src/player_sync.cpp` — dispatch `SNAPSHOT_UPLOAD_ACK`; tick uploader each frame
- Modify: `core/src/plugin.cpp` or a new glue file — build the `SnapshotUploader::Deps` with real bindings (`save_trigger_*`, `zip_directory`, `client_send_reliable`, clock via `GetTickCount64`), own the `std::thread` for the async zip

Because the real dependencies need a `std::thread` + `std::atomic<bool>` + a resulting blob buffer, the glue isn't a pure one-liner. Put it in a new file: `core/src/snapshot_uploader_glue.{h,cpp}`.

- [ ] **Step 1: Create `core/src/snapshot_uploader_glue.h`**

```cpp
// snapshot_uploader_glue.h — Real Kenshi / ENet / threading bindings for
// the SnapshotUploader. This is the only file in the plugin that wires
// the uploader to its world.
#pragma once

#include <string>

namespace kmp {

/// Initialise the singleton uploader with real bindings. Call once from
/// plugin startup.
void snapshot_uploader_glue_init();

/// Shut down: join any background threads, clear state.
void snapshot_uploader_glue_shutdown();

/// Begin upload of the named slot. Called when we confirm the local player
/// is the host (from ui_on_connect_accept).
void snapshot_uploader_glue_start(const std::string& slot);

/// Called every frame from player_sync_tick.
void snapshot_uploader_glue_tick(float dt);

/// Forwards an incoming SNAPSHOT_UPLOAD_ACK from the receive path.
struct SnapshotUploadAck;
void snapshot_uploader_glue_on_ack(const SnapshotUploadAck& ack);

/// For UI — empty string if idle or no-progress.
std::string snapshot_uploader_glue_progress_text();

} // namespace kmp
```

- [ ] **Step 2: Create `core/src/snapshot_uploader_glue.cpp`**

```cpp
#include "snapshot_uploader_glue.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "packets.h"
#include "save_trigger.h"
#include "snapshot_uploader.h"
#include "snapshot_zip.h"

namespace kmp {

namespace {

struct ZipJob {
    std::thread              worker;
    std::atomic<bool>        done{false};
    std::vector<uint8_t>     result;
    std::mutex               mu;
};

static std::unique_ptr<SnapshotUploader> s_uploader;
static std::unique_ptr<ZipJob>           s_zip_job;

// Forward-declare the plugin-side ENet send (defined in client.cpp).
} // namespace

extern void client_send_reliable(const uint8_t* data, size_t length);

namespace {

static float clock_seconds() {
    static auto t0 = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::duration<float>>(now - t0).count();
}

static bool send_reliable_shim(const uint8_t* data, size_t len) {
    client_send_reliable(data, len);
    return true;  // existing API is void; treat as always succeeding
}

static void start_zip_bg(const std::string& abs_path) {
    if (!s_zip_job) s_zip_job.reset(new ZipJob());
    // Reset for a fresh run.
    s_zip_job->done.store(false);
    {
        std::lock_guard<std::mutex> lk(s_zip_job->mu);
        s_zip_job->result.clear();
    }
    if (s_zip_job->worker.joinable()) s_zip_job->worker.join();
    s_zip_job->worker = std::thread([path = abs_path]() {
        std::vector<uint8_t> blob = zip_directory(path);
        {
            std::lock_guard<std::mutex> lk(s_zip_job->mu);
            s_zip_job->result = std::move(blob);
        }
        s_zip_job->done.store(true);
    });
}

static bool poll_zip_bg(std::vector<uint8_t>& out) {
    if (!s_zip_job) return false;
    if (!s_zip_job->done.load()) return false;
    if (s_zip_job->worker.joinable()) s_zip_job->worker.join();
    std::lock_guard<std::mutex> lk(s_zip_job->mu);
    out = std::move(s_zip_job->result);
    s_zip_job->done.store(false);  // reset so a later run works
    return true;
}

} // namespace

void snapshot_uploader_glue_init() {
    SnapshotUploader::Deps d;
    d.trigger_save       = [](const std::string& slot) { return save_trigger_start(slot); };
    d.is_save_busy       = []() { return save_trigger_is_busy(); };
    d.resolve_slot_path  = [](const std::string& slot) { return save_trigger_resolve_slot_path(slot); };
    d.start_zip          = [](const std::string& path) { start_zip_bg(path); };
    d.poll_zip           = [](std::vector<uint8_t>& out) { return poll_zip_bg(out); };
    d.send_reliable      = [](const uint8_t* data, size_t len) { return send_reliable_shim(data, len); };
    d.now_seconds        = []() { return clock_seconds(); };

    s_uploader.reset(new SnapshotUploader(std::move(d)));
}

void snapshot_uploader_glue_shutdown() {
    if (s_zip_job && s_zip_job->worker.joinable()) s_zip_job->worker.join();
    s_zip_job.reset();
    s_uploader.reset();
}

void snapshot_uploader_glue_start(const std::string& slot) {
    if (!s_uploader) return;
    s_uploader->start(slot);
}

void snapshot_uploader_glue_tick(float dt) {
    if (!s_uploader) return;
    s_uploader->tick(dt);
}

void snapshot_uploader_glue_on_ack(const SnapshotUploadAck& ack) {
    if (!s_uploader) return;
    s_uploader->on_ack(ack);
}

std::string snapshot_uploader_glue_progress_text() {
    if (!s_uploader) return {};
    return s_uploader->progress_text();
}

} // namespace kmp
```

- [ ] **Step 3: Register source + init in plugin**

Add `src/snapshot_uploader_glue.cpp` to `core/CMakeLists.txt`'s source list.

In `core/src/plugin.cpp`, add to the `namespace kmp` forward-decls block:

```cpp
    void snapshot_uploader_glue_init();
    void snapshot_uploader_glue_shutdown();
```

In the `hooked_main_loop` function's "Defer subsystem init" block, after `kmp::ui_init();` add:

```cpp
        kmp::snapshot_uploader_glue_init();
```

(No shutdown hook is wired today for the other subsystems — `ui_shutdown` etc. aren't called. Leave the `_shutdown` declared for future use; it's harmless to add.)

- [ ] **Step 4: Start upload from ui.cpp on host connect**

In `core/src/ui.cpp`, near the top of the file in the `extern` block (right after `extern bool host_sync_is_host();`), add:

```cpp
extern void snapshot_uploader_glue_start(const std::string& slot);
extern std::string snapshot_uploader_glue_progress_text();
```

Find `ui_on_connect_accept`. After the existing body (after `update_status_text()` and the `s_chat_window` show logic), append:

```cpp
    if (host_sync_is_host()) {
        KMP_LOG("[KenshiMP] host connected — beginning snapshot upload");
        snapshot_uploader_glue_start("KMP_Session");
    }
```

- [ ] **Step 5: Show progress in the status bar**

In `core/src/ui.cpp::update_status_text()` (around line 850–872), extend the connected-host path so it appends the uploader's progress text:

```cpp
static void update_status_text() {
    if (!s_status_text) return;

    if (client_is_connected()) {
        std::string role = host_sync_is_host() ? "HOSTING" : "JOINED";
        std::string base = "KenshiMP - " + role + " as Player #" + itos(client_get_local_id());
        std::string extra = snapshot_uploader_glue_progress_text();
        if (!extra.empty()) {
            base += "  ·  " + extra;
        }
        s_status_text->setCaption(base);
        s_status_text->setTextColour(MyGUI::Colour(0.4f, 1.0f, 0.4f));
    } else {
        s_status_text->setCaption("KenshiMP - Disconnected  (F8: connect)");
        s_status_text->setTextColour(MyGUI::Colour(1.0f, 0.6f, 0.6f));
    }
}
```

Also drive `update_status_text()` to be called each frame (so progress numbers refresh). In `player_sync.cpp::player_sync_tick`, after the existing `ui_update_main_menu_button();` call, add a call to update_status_text (requires exposing it). Actually simpler: expose a `ui_tick()` that does both. But to minimise surface: make `update_status_text` non-static and call it directly.

Find the `static void update_status_text();` forward declaration near the top of `ui.cpp` and remove the `static` qualifier. Same for the definition. Rename to `ui_update_status_text` to avoid name collision with anything else. Find all callers (there are ~4 inside ui.cpp) and update.

In `player_sync.cpp`, next to `ui_update_main_menu_button();`, add:

```cpp
extern void ui_update_status_text();
// ...
ui_update_status_text();
```

- [ ] **Step 6: Tick + dispatch in player_sync.cpp**

In `core/src/player_sync.cpp`, add to the `extern` block near the top:

```cpp
extern void snapshot_uploader_glue_tick(float dt);
extern void snapshot_uploader_glue_on_ack(const SnapshotUploadAck& ack);
```

In `player_sync_tick`, right after the existing `ui_check_hotkey(); ui_update_main_menu_button();` lines, add:

```cpp
    snapshot_uploader_glue_tick(dt);
```

In the packet-dispatch `switch` (around line 156), add a new case (before `default`):

```cpp
    case PacketType::SNAPSHOT_UPLOAD_ACK: {
        SnapshotUploadAck pkt;
        if (unpack(data, length, pkt)) {
            snapshot_uploader_glue_on_ack(pkt);
        }
        break;
    }
```

- [ ] **Step 7: Build + test**

```bash
make core 2>&1 | tail -10
make test 2>&1 | tail -10
```

Both should complete cleanly. `make test` runs unit tests only (no Kenshi needed) — they all still pass since this task only modifies the plugin.

- [ ] **Step 8: Commit**

```bash
git add core/src/snapshot_uploader_glue.h core/src/snapshot_uploader_glue.cpp \
        core/src/ui.cpp core/src/player_sync.cpp core/src/plugin.cpp \
        core/CMakeLists.txt
git commit -m "feat(core): wire SnapshotUploader into plugin (start on host, tick, ACK)"
```

---

## Task 6: Chat log + deploy + manual smoke test

**Files:**
- Modify: `core/src/ui.cpp` — emit chat lines on state transitions

And a documented manual smoke test. No code changes in Kenshi are testable automatically — this is the moment of truth.

- [ ] **Step 1: Emit chat log lines on state transitions**

The uploader reports its state via `state()`. Track the previous state in `ui.cpp` and emit `append_system_message(...)` when a meaningful transition happens:

In `core/src/ui.cpp`, add near the other statics:

```cpp
static int s_last_upload_state = -1;  // matches SnapshotUploader::State enum int values
```

Expose the state() accessor through the glue. Add to `core/src/snapshot_uploader_glue.h`:

```cpp
int snapshot_uploader_glue_state_int();
```

And in `snapshot_uploader_glue.cpp`:

```cpp
int snapshot_uploader_glue_state_int() {
    if (!s_uploader) return -1;
    return static_cast<int>(s_uploader->state());
}
std::string snapshot_uploader_glue_last_error() {
    if (!s_uploader) return {};
    return s_uploader->last_error();
}
```

And a header declaration:

```cpp
std::string snapshot_uploader_glue_last_error();
```

In `ui.cpp`, add corresponding externs:

```cpp
extern int snapshot_uploader_glue_state_int();
extern std::string snapshot_uploader_glue_last_error();
```

Add a new helper function in `ui.cpp`:

```cpp
// Map SnapshotUploader::State values (order must match the enum) to chat lines.
static void ui_poll_upload_state_changes() {
    int st = snapshot_uploader_glue_state_int();
    if (st == s_last_upload_state || st < 0) return;
    s_last_upload_state = st;
    switch (st) {
        case 0: /* IDLE */
            // If we got here from AWAIT_ACK, it means success.
            append_system_message("World ready — joiners can connect");
            break;
        case 1: /* WAIT_SAVE */
            append_system_message("Preparing world snapshot...");
            break;
        case 2: /* ZIP_RUNNING */
            append_system_message("Packaging world...");
            break;
        case 3: /* SEND_CHUNKS */
            append_system_message("Uploading world to server...");
            break;
        case 5: /* FAILED */
            append_system_message(std::string("Upload failed: ")
                + snapshot_uploader_glue_last_error());
            break;
        default:
            break;
    }
}
```

Wire it into the per-frame tick: in `ui.cpp`, expose `ui_poll_upload_state_changes` as non-static (or add a new wrapper) and call it from `player_sync.cpp` just after `snapshot_uploader_glue_tick(dt);`.

Actually simpler: since `ui_update_status_text` is already called every frame from `player_sync_tick`, just call `ui_poll_upload_state_changes()` at the top of `ui_update_status_text()`. Same life cycle, zero extra plumbing.

- [ ] **Step 2: Build + deploy**

```bash
make deploy 2>&1 | tail -10
```

Expected: clean build + copy to the Kenshi mods folder.

- [ ] **Step 3: Manual smoke test — small save**

1. Launch Kenshi normally.
2. Load an existing save (or new-game — anything < 10 MB on disk).
3. Click Multiplayer → Host (F8 or the main-menu button).
4. Watch the status bar top-left and the chat window.
5. Expected sequence within ~10 seconds:
   - Status: `KenshiMP - HOSTING as Player #1  ·  Hosting: saving world...`
   - Chat: `Preparing world snapshot...`
   - Status: `Hosting: packaging world...`
   - Chat: `Packaging world...`
   - Status: `Hosting: uploading 0.3 / 3.2 MB` (updates as chunks go)
   - Chat: `Uploading world to server...`
   - Status: `Hosting: finalising...`
   - Status: `KenshiMP - HOSTING as Player #1` (no suffix = idle/ok)
   - Chat: `World ready — joiners can connect`
6. Open a second terminal:
   ```bash
   curl -i http://127.0.0.1:7778/snapshot -o /tmp/out.zip
   unzip -l /tmp/out.zip | head
   ```
   Expected: `HTTP/1.1 200 OK`, `Content-Type: application/zip`, and the unzip listing shows files that look like Kenshi save files (`platoon`, `save.xml`, etc.).

- [ ] **Step 4: Manual smoke test — failure path**

1. With Kenshi closed, rename the deployed `KMP_Session` save folder to `KMP_Session.bak` (in `%USERPROFILE%\Documents\My Games\Kenshi\save\`).
2. Start Kenshi, click Host.
3. The save will be created from scratch (should succeed). If you want to force a failure, instead make the save folder read-only — then `saveGame` should fail and status should show `Hosting: upload failed (save refused)`.
4. Restore the folder afterwards.

This is optional — the failure path is well covered by unit tests.

- [ ] **Step 5: Commit**

```bash
git add core/src/ui.cpp core/src/snapshot_uploader_glue.h core/src/snapshot_uploader_glue.cpp
git commit -m "feat(core): chat log feedback for snapshot-upload state transitions"
```

---

## Final verification

All unit tests pass + manual smoke passes.

```bash
make test
# → All snapshot unit tests passed.
```

Then deploy + do smoke test 1 (small save). Merge the branch when both tick.

At that point Plan A.2 is shipped: host uploads their world to the server. Plan A.3 (joiner-side download + load + menu UX) can begin — the HTTP sidecar now serves real data.

## Self-review notes

- **Spec coverage:**
  - "Source slot" → Task 3 (save_trigger resolves path, writes to `KMP_Session`)
  - "Worker thread for zip" → Task 5 (`snapshot_uploader_glue.cpp::start_zip_bg`)
  - "Dependency-injected uploader for testing" → Task 4
  - "State machine w/ timeouts" → Task 4 (kSaveTimeoutSec, kAckTimeoutSec)
  - "UI progress text + chat log" → Task 5 (progress_text) + Task 6 (state-change chat lines)
  - "Failure handling matrix" → Task 4 tests cover each row
  - "Unit tests: snapshot_zip + snapshot_uploader" → Task 2 + Task 4
  - "Manual integration: curl /snapshot" → Task 6 Step 3
- **Placeholders:** none. Every step has code, every assertion is concrete.
- **Type consistency:** `SnapshotUploader::State` enum values: tests use `WAIT_SAVE` after start → matches impl (start goes directly to WAIT_SAVE, not TRIGGER_SAVE). Fixed in the header — there's no separate TRIGGER_SAVE state in the machine, since `trigger_save` is called synchronously inside `start()`.

  Cross-reference: spec's state diagram has `TRIGGER_SAVE` as a distinct state. The simpler impl — one less transition — merges it with `start()`. Tests and code are consistent. The spec should note this, but it's a minor simplification that doesn't change behaviour.
- **Scope:** focused — one module per task. No unrelated refactors.
