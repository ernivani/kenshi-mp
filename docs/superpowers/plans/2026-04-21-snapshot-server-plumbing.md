# Snapshot Server Plumbing (Plan A.1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the server-side machinery for receiving a host's save snapshot over ENet and serving it to joiners over HTTP. After this plan ships, a fake host (the test harness) can upload a zip blob to the server, and a plain HTTP client can download it back — with sha256 integrity, rev counter, and If-None-Match support.

**Architecture:** Three new components in `kenshi-mp-server-core`: an in-RAM snapshot store (thread-safe, rev counter, sha256), an upload session tracker that reassembles ENet chunks into the store, and an HTTP sidecar (cpp-httplib) that exposes `GET /snapshot` on `port+1`. Three new ENet packet types (`SNAPSHOT_UPLOAD_BEGIN / CHUNK / END`) follow the existing fixed-prefix-plus-variable-tail pattern for the chunk payload.

**Tech Stack:** C++17, existing server toolchain (MSVC + CMake). New vendored headers: `cpp-httplib` (HTTP server/client, MIT), `picosha2` (sha256, public domain). Both single-header, placed under `deps/`.

**Parent spec:** `docs/superpowers/specs/2026-04-21-save-transfer-and-load-design.md`

**Out of scope (separate plans A.2, A.3):** Host-side save trigger hook, zip producer, ENet uploader, joiner-side HTTP download UI, zip extractor, `importGame` call, menu rework.

**Worktree note:** This plan does not require a dedicated worktree — all changes are additive (new files + small additions to existing). If the agent is running this with an isolation worktree, that's fine too.

---

## File Structure

**New files:**
- `deps/cpp-httplib/httplib.h` — vendored single-header
- `deps/picosha2/picosha2.h` — vendored single-header
- `common/include/packets.h` — new packet types + chunk helper (modified, not created)
- `common/include/serialization.h` — add variable-length pack helper (modified)
- `server/core/src/snapshot.h` / `snapshot.cpp` — snapshot store
- `server/core/src/snapshot_upload.h` / `snapshot_upload.cpp` — reassembly of uploaded chunks
- `server/core/src/http_sidecar.h` / `http_sidecar.cpp` — cpp-httplib wrapping
- `tools/test_snapshot.cpp` — end-to-end integration test exe

**Modified:**
- `server/core/CMakeLists.txt` — add new sources + httplib include path
- `server/core/src/core.cpp` — wire snapshot store + sidecar into init/shutdown
- `server/core/src/session.cpp` — handle SNAPSHOT_UPLOAD_* packets
- `common/include/protocol.h` — if the packet size constants need updating (check during Task 2)
- `tools/CMakeLists.txt` — add test_snapshot target

**Responsibility split:**
- `snapshot.{h,cpp}`: owns the current-snapshot blob + rev + sha. Thread-safe getters/setters. No networking.
- `snapshot_upload.{h,cpp}`: per-peer in-progress upload tracking. Receives chunks, reassembles, verifies sha, commits to `snapshot`. No networking layer knowledge (accepts decoded packets).
- `http_sidecar.{h,cpp}`: cpp-httplib wrapper. Binds port+1, serves GET /snapshot from `snapshot` store. Runs on its own thread.
- `session.cpp` diff: tiny glue — dispatch SNAPSHOT_UPLOAD_* packets to `snapshot_upload`.

---

## Task 1: Vendor cpp-httplib and picosha2

**Files:**
- Create: `deps/cpp-httplib/httplib.h`
- Create: `deps/cpp-httplib/LICENSE`
- Create: `deps/picosha2/picosha2.h`
- Create: `deps/picosha2/LICENSE`

Both libraries are single-header. We vendor them to avoid network fetches at build time and to pin versions.

- [ ] **Step 1: Download cpp-httplib v0.15.3**

The `httplib.h` is at `https://github.com/yhirose/cpp-httplib/raw/v0.15.3/httplib.h` (approximately 200 KB). Download with curl or PowerShell:

```bash
curl -L -o deps/cpp-httplib/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h
curl -L -o deps/cpp-httplib/LICENSE \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/LICENSE
```

Verify file size is 150-250 KB and LICENSE contains the MIT license text.

- [ ] **Step 2: Download picosha2**

```bash
curl -L -o deps/picosha2/picosha2.h \
  https://raw.githubusercontent.com/okdshin/PicoSHA2/master/picosha2.h
```

Picosha2 declares its license at the top of the header (zlib-ish, attribution-only). Save a copy of the header comment block as `deps/picosha2/LICENSE`:

```bash
head -30 deps/picosha2/picosha2.h > deps/picosha2/LICENSE
```

Verify `picosha2.h` is around 15-30 KB and contains `namespace picosha2`.

- [ ] **Step 3: Commit vendored deps**

```bash
git add deps/cpp-httplib deps/picosha2
git commit -m "deps: vendor cpp-httplib v0.15.3 and picosha2"
```

---

## Task 2: Add SNAPSHOT_UPLOAD_* packet types

**Files:**
- Modify: `common/include/packets.h` (PacketType block + 3 new structs)
- Modify: `common/include/serialization.h` (add `pack_with_tail` helper)

The three new packets:
- `SNAPSHOT_UPLOAD_BEGIN` — fixed struct: `{header, upload_id, rev, total_size, sha256[32]}`. `upload_id` is chosen by the sender and echoed in CHUNK/END so multiple concurrent uploads wouldn't collide (future-proofing, MVP supports one at a time).
- `SNAPSHOT_UPLOAD_CHUNK` — variable length: `{header, upload_id, offset, length}` followed by `length` raw bytes. Max `length` = 61440 (60 KB) to keep total packet under ENet's practical 64 KB limit with room for header and other framing.
- `SNAPSHOT_UPLOAD_END` — fixed struct: `{header, upload_id}`. Signals "all chunks sent, please verify and commit."

Also add one server→host ack: `SNAPSHOT_UPLOAD_ACK` — `{header, upload_id, accepted, error_code}`. Simple 1-byte `accepted` field, `error_code` for "sha mismatch" / "out-of-order" / "size mismatch". Host logs on failure, user-facing error later in Plan A.2.

- [ ] **Step 1: Write the failing tests for pack/unpack round-trip**

Create `tools/test_snapshot_packets.cpp`:

```cpp
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "packets.h"
#include "serialization.h"

using namespace kmp;

static void test_upload_begin_roundtrip() {
    SnapshotUploadBegin orig;
    orig.upload_id  = 0x12345678;
    orig.rev        = 7;
    orig.total_size = 1024 * 1024;
    for (int i = 0; i < 32; ++i) orig.sha256[i] = static_cast<uint8_t>(i * 3);

    auto buf = pack(orig);

    SnapshotUploadBegin got;
    bool ok = unpack(buf.data(), buf.size(), got);
    assert(ok);
    assert(got.upload_id  == 0x12345678);
    assert(got.rev        == 7);
    assert(got.total_size == 1024 * 1024);
    for (int i = 0; i < 32; ++i) assert(got.sha256[i] == static_cast<uint8_t>(i * 3));
    printf("test_upload_begin_roundtrip OK\n");
}

static void test_upload_chunk_roundtrip() {
    std::vector<uint8_t> data(8192);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    SnapshotUploadChunk chunk;
    chunk.upload_id = 99;
    chunk.offset    = 65536;
    chunk.length    = static_cast<uint16_t>(data.size());

    auto buf = pack_with_tail(chunk, data.data(), data.size());

    SnapshotUploadChunk got_hdr;
    const uint8_t* tail = nullptr;
    size_t tail_len = 0;
    bool ok = unpack_with_tail(buf.data(), buf.size(), got_hdr, tail, tail_len);
    assert(ok);
    assert(got_hdr.upload_id == 99);
    assert(got_hdr.offset    == 65536);
    assert(got_hdr.length    == data.size());
    assert(tail_len == data.size());
    assert(std::memcmp(tail, data.data(), data.size()) == 0);
    printf("test_upload_chunk_roundtrip OK\n");
}

static void test_upload_end_roundtrip() {
    SnapshotUploadEnd orig;
    orig.upload_id = 42;
    auto buf = pack(orig);
    SnapshotUploadEnd got;
    bool ok = unpack(buf.data(), buf.size(), got);
    assert(ok);
    assert(got.upload_id == 42);
    printf("test_upload_end_roundtrip OK\n");
}

int main() {
    test_upload_begin_roundtrip();
    test_upload_chunk_roundtrip();
    test_upload_end_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
```

Add a target in `tools/CMakeLists.txt`:

```cmake
add_executable(test-snapshot-packets test_snapshot_packets.cpp)
target_link_libraries(test-snapshot-packets PRIVATE kenshi-mp-common)
```

- [ ] **Step 2: Run test to verify it fails (build error, types missing)**

```bash
cmake --build build_server --target test-snapshot-packets
```

Expected: compile errors — `SnapshotUploadBegin`, `pack_with_tail`, `unpack_with_tail` are not declared.

- [ ] **Step 3: Add new packet types to packets.h**

In `common/include/packets.h`, add inside `namespace PacketType` block (after line 43):

```cpp
    // Host uploads the current save snapshot to the server. Multi-chunk,
    // sha256-verified. See docs/superpowers/specs/2026-04-21-save-transfer-and-load-design.md
    static const uint8_t SNAPSHOT_UPLOAD_BEGIN = 0xA0;
    static const uint8_t SNAPSHOT_UPLOAD_CHUNK = 0xA1;
    static const uint8_t SNAPSHOT_UPLOAD_END   = 0xA2;
    static const uint8_t SNAPSHOT_UPLOAD_ACK   = 0xA3;
```

Then add the struct declarations at the bottom of the file, before the closing `}` of the `kmp` namespace:

```cpp
struct SnapshotUploadBegin {
    PacketHeader header;
    uint32_t     upload_id;
    uint32_t     rev;
    uint64_t     total_size;
    uint8_t      sha256[32];

    SnapshotUploadBegin() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_BEGIN;
    }
};

// Wire layout: {PacketHeader, upload_id, offset, length}{<length> bytes}.
// Serialize via pack_with_tail / deserialize via unpack_with_tail.
struct SnapshotUploadChunk {
    PacketHeader header;
    uint32_t     upload_id;
    uint32_t     offset;
    uint16_t     length;

    SnapshotUploadChunk() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_CHUNK;
    }
};

struct SnapshotUploadEnd {
    PacketHeader header;
    uint32_t     upload_id;

    SnapshotUploadEnd() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_END;
    }
};

struct SnapshotUploadAck {
    PacketHeader header;
    uint32_t     upload_id;
    uint8_t      accepted;    // 0 = rejected, 1 = accepted
    uint8_t      error_code;  // 0 = none, 1 = sha_mismatch, 2 = size_mismatch, 3 = out_of_order, 4 = no_upload
    uint16_t     _pad;

    SnapshotUploadAck() {
        std::memset(this, 0, sizeof(*this));
        header.version = PROTOCOL_VERSION;
        header.type    = PacketType::SNAPSHOT_UPLOAD_ACK;
    }
};
```

- [ ] **Step 4: Add variable-length serialization helpers to serialization.h**

In `common/include/serialization.h`, add after `unpack`:

```cpp
/// Pack a fixed-size struct followed by a variable-length byte tail.
/// Used for SnapshotUploadChunk and any future variable-length packets.
template <typename T>
inline std::vector<uint8_t> pack_with_tail(const T& packet, const uint8_t* tail, size_t tail_len) {
    std::vector<uint8_t> buf(sizeof(T) + tail_len);
    std::memcpy(buf.data(), &packet, sizeof(T));
    if (tail_len > 0 && tail) {
        std::memcpy(buf.data() + sizeof(T), tail, tail_len);
    }
    return buf;
}

/// Inverse of pack_with_tail: writes the fixed prefix into `out`, and sets
/// `tail_ptr` and `tail_len` to point to the trailing bytes inside `data`.
/// The tail is NOT copied — `data` must outlive the caller's use of `tail_ptr`.
/// Returns true on success, false if `length` is too small.
template <typename T>
inline bool unpack_with_tail(const uint8_t* data, size_t length, T& out,
                             const uint8_t*& tail_ptr, size_t& tail_len) {
    if (length < sizeof(T)) return false;
    std::memcpy(&out, data, sizeof(T));
    tail_ptr = data + sizeof(T);
    tail_len = length - sizeof(T);
    return true;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build build_server --target test-snapshot-packets
./build_server/bin/Release/test-snapshot-packets.exe
```

Expected output:

```
test_upload_begin_roundtrip OK
test_upload_chunk_roundtrip OK
test_upload_end_roundtrip OK
ALL PASS
```

- [ ] **Step 6: Commit**

```bash
git add common/include/packets.h common/include/serialization.h tools/test_snapshot_packets.cpp tools/CMakeLists.txt
git commit -m "feat(common): add SNAPSHOT_UPLOAD_* packet types + pack_with_tail helper"
```

---

## Task 3: Server-side snapshot store

**Files:**
- Create: `server/core/src/snapshot.h`
- Create: `server/core/src/snapshot.cpp`
- Test: `tools/test_snapshot_store.cpp`

The store owns the current snapshot blob in RAM, its rev counter, and its sha256. Thread-safe because the HTTP sidecar reads it on its own thread while ENet handlers may swap it on another.

- [ ] **Step 1: Write the failing tests**

Create `tools/test_snapshot_store.cpp`:

```cpp
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "snapshot.h"

using namespace kmp;

static void test_empty_store_has_no_snapshot() {
    SnapshotStore store;
    std::vector<uint8_t> blob;
    uint32_t rev = 0;
    assert(!store.get(blob, rev));
    assert(rev == 0);
    printf("test_empty_store_has_no_snapshot OK\n");
}

static void test_set_and_get_snapshot() {
    SnapshotStore store;
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    uint8_t sha[32] = {};
    for (int i = 0; i < 32; ++i) sha[i] = static_cast<uint8_t>(i);

    store.set(std::move(payload), sha);

    std::vector<uint8_t> got_blob;
    uint32_t got_rev = 0;
    assert(store.get(got_blob, got_rev));
    assert(got_rev == 1);
    assert(got_blob.size() == 5);
    assert(got_blob[0] == 1);
    assert(got_blob[4] == 5);

    uint8_t got_sha[32] = {};
    store.get_sha(got_sha);
    for (int i = 0; i < 32; ++i) assert(got_sha[i] == static_cast<uint8_t>(i));

    printf("test_set_and_get_snapshot OK\n");
}

static void test_rev_increments_on_set() {
    SnapshotStore store;
    uint8_t sha[32] = {};
    store.set({1, 2, 3}, sha);
    store.set({4, 5, 6}, sha);
    store.set({7}, sha);
    std::vector<uint8_t> blob;
    uint32_t rev = 0;
    assert(store.get(blob, rev));
    assert(rev == 3);
    assert(blob.size() == 1 && blob[0] == 7);
    printf("test_rev_increments_on_set OK\n");
}

int main() {
    test_empty_store_has_no_snapshot();
    test_set_and_get_snapshot();
    test_rev_increments_on_set();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-snapshot-store test_snapshot_store.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot.cpp)
target_include_directories(test-snapshot-store PRIVATE
    ${CMAKE_SOURCE_DIR}/server/core/src)
target_link_libraries(test-snapshot-store PRIVATE kenshi-mp-common)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build_server --target test-snapshot-store
```

Expected: `snapshot.h` not found.

- [ ] **Step 3: Create snapshot.h**

```cpp
// snapshot.h — In-RAM store for the current save snapshot.
//
// Thread-safe. One producer (host-upload handler on the ENet thread) swaps
// the blob; one or more consumers (HTTP sidecar serving GET /snapshot) read
// it. No historical snapshots — only the latest.

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace kmp {

class SnapshotStore {
public:
    /// Replace the current snapshot with `blob`. `sha256` is a 32-byte digest
    /// of `blob` that will be returned to future callers. Increments rev.
    void set(std::vector<uint8_t> blob, const uint8_t sha256[32]);

    /// Copy the current snapshot + rev into the out-params. Returns false if
    /// no snapshot has been uploaded yet.
    bool get(std::vector<uint8_t>& out_blob, uint32_t& out_rev) const;

    /// Write the current sha256 into `out_sha[32]`. Zeroes it if no snapshot.
    void get_sha(uint8_t out_sha[32]) const;

    /// Return the current rev, or 0 if no snapshot. Cheap.
    uint32_t rev() const;

    /// True if at least one snapshot has been uploaded.
    bool has_snapshot() const;

private:
    mutable std::mutex m_mu;
    std::vector<uint8_t> m_blob;
    uint8_t m_sha[32] = {};
    uint32_t m_rev = 0;
};

} // namespace kmp
```

- [ ] **Step 4: Create snapshot.cpp**

```cpp
#include "snapshot.h"
#include <cstring>

namespace kmp {

void SnapshotStore::set(std::vector<uint8_t> blob, const uint8_t sha256[32]) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_blob = std::move(blob);
    std::memcpy(m_sha, sha256, 32);
    ++m_rev;
}

bool SnapshotStore::get(std::vector<uint8_t>& out_blob, uint32_t& out_rev) const {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_rev == 0) {
        out_blob.clear();
        out_rev = 0;
        return false;
    }
    out_blob = m_blob;
    out_rev = m_rev;
    return true;
}

void SnapshotStore::get_sha(uint8_t out_sha[32]) const {
    std::lock_guard<std::mutex> lk(m_mu);
    std::memcpy(out_sha, m_sha, 32);
}

uint32_t SnapshotStore::rev() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_rev;
}

bool SnapshotStore::has_snapshot() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_rev > 0;
}

} // namespace kmp
```

- [ ] **Step 5: Run tests**

```bash
cmake --build build_server --target test-snapshot-store
./build_server/bin/Release/test-snapshot-store.exe
```

Expected:

```
test_empty_store_has_no_snapshot OK
test_set_and_get_snapshot OK
test_rev_increments_on_set OK
ALL PASS
```

- [ ] **Step 6: Commit**

```bash
git add server/core/src/snapshot.h server/core/src/snapshot.cpp tools/test_snapshot_store.cpp tools/CMakeLists.txt
git commit -m "feat(server): add in-RAM SnapshotStore"
```

---

## Task 4: Snapshot upload reassembly

**Files:**
- Create: `server/core/src/snapshot_upload.h`
- Create: `server/core/src/snapshot_upload.cpp`
- Test: `tools/test_snapshot_upload.cpp`

This component accepts decoded `SnapshotUploadBegin` / `Chunk` / `End` packet payloads and, on successful `End`, verifies the sha256 and commits to a `SnapshotStore`. It tracks one in-progress upload at a time (keyed by `upload_id`). New BEGIN replaces any in-progress upload (host can retry cleanly).

- [ ] **Step 1: Write the failing tests**

Create `tools/test_snapshot_upload.cpp`:

```cpp
#include <cassert>
#include <cstdio>
#include <vector>
#include "snapshot.h"
#include "snapshot_upload.h"
#include "picosha2.h"

using namespace kmp;

static std::vector<uint8_t> make_blob(size_t n, uint8_t seed) {
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>((i + seed) & 0xFF);
    return b;
}

static void sha_of(const std::vector<uint8_t>& blob, uint8_t out[32]) {
    picosha2::hash256(blob.begin(), blob.end(), out, out + 32);
}

static void test_successful_upload() {
    SnapshotStore store;
    SnapshotUploadSession sess(store);

    auto payload = make_blob(8192, 7);
    uint8_t sha[32];
    sha_of(payload, sha);

    bool ok = sess.on_begin(/*upload_id*/ 1, /*rev*/ 42, payload.size(), sha);
    assert(ok);

    // Send in two chunks: 0..4095, 4096..8191
    ok = sess.on_chunk(1, 0,    payload.data(),        4096);  assert(ok);
    ok = sess.on_chunk(1, 4096, payload.data() + 4096, 4096);  assert(ok);

    SnapshotUploadResult res = sess.on_end(1);
    assert(res == SnapshotUploadResult::Committed);
    assert(store.rev() == 42);

    std::vector<uint8_t> got; uint32_t r;
    assert(store.get(got, r));
    assert(got == payload);

    printf("test_successful_upload OK\n");
}

static void test_sha_mismatch_rejects() {
    SnapshotStore store;
    SnapshotUploadSession sess(store);

    auto payload = make_blob(1024, 1);
    uint8_t wrong_sha[32] = {};
    for (int i = 0; i < 32; ++i) wrong_sha[i] = 0xAA;

    sess.on_begin(5, 1, payload.size(), wrong_sha);
    sess.on_chunk(5, 0, payload.data(), static_cast<uint16_t>(payload.size()));
    SnapshotUploadResult res = sess.on_end(5);
    assert(res == SnapshotUploadResult::ShaMismatch);
    assert(!store.has_snapshot());  // store unchanged

    printf("test_sha_mismatch_rejects OK\n");
}

static void test_size_mismatch_rejects() {
    SnapshotStore store;
    SnapshotUploadSession sess(store);

    uint8_t sha[32] = {};
    sess.on_begin(6, 1, 2048, sha);
    auto payload = make_blob(1024, 1);
    sess.on_chunk(6, 0, payload.data(), static_cast<uint16_t>(payload.size()));
    SnapshotUploadResult res = sess.on_end(6);
    assert(res == SnapshotUploadResult::SizeMismatch);
    assert(!store.has_snapshot());

    printf("test_size_mismatch_rejects OK\n");
}

static void test_new_begin_discards_in_progress() {
    SnapshotStore store;
    SnapshotUploadSession sess(store);
    uint8_t sha[32] = {};

    sess.on_begin(10, 1, 4096, sha);
    sess.on_chunk(10, 0, make_blob(2048, 0).data(), 2048);
    // Now another BEGIN with a different id. The old one is discarded.
    sess.on_begin(11, 2, 1024, sha);

    // Chunk for the old id should be rejected (no matching session).
    bool ok = sess.on_chunk(10, 2048, make_blob(2048, 0).data(), 2048);
    assert(!ok);

    printf("test_new_begin_discards_in_progress OK\n");
}

int main() {
    test_successful_upload();
    test_sha_mismatch_rejects();
    test_size_mismatch_rejects();
    test_new_begin_discards_in_progress();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-snapshot-upload test_snapshot_upload.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot_upload.cpp)
target_include_directories(test-snapshot-upload PRIVATE
    ${CMAKE_SOURCE_DIR}/server/core/src
    ${CMAKE_SOURCE_DIR}/deps/picosha2)
target_link_libraries(test-snapshot-upload PRIVATE kenshi-mp-common)
```

- [ ] **Step 2: Run test to verify failure**

```bash
cmake --build build_server --target test-snapshot-upload
```

Expected: `snapshot_upload.h` not found.

- [ ] **Step 3: Create snapshot_upload.h**

```cpp
// snapshot_upload.h — Reassembles an incoming SNAPSHOT_UPLOAD_* sequence
// from the host into a complete blob, verifies sha256, and commits to the
// SnapshotStore. Tracks one in-progress upload at a time.

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace kmp {

class SnapshotStore;

enum class SnapshotUploadResult {
    Committed,       // sha+size OK, snapshot accepted
    NoUpload,        // on_end called with no matching BEGIN
    SizeMismatch,    // total bytes != declared total_size
    ShaMismatch,     // sha256(blob) != declared sha256
};

class SnapshotUploadSession {
public:
    explicit SnapshotUploadSession(SnapshotStore& store);

    /// Start (or restart) an upload. Any in-progress upload is discarded.
    /// Returns false if `total_size` is unreasonable (e.g. > 512 MB).
    bool on_begin(uint32_t upload_id, uint32_t rev,
                  uint64_t total_size, const uint8_t sha256[32]);

    /// Deliver a chunk. Returns false if no upload is in progress, the
    /// upload_id doesn't match, or the chunk lies outside declared bounds.
    bool on_chunk(uint32_t upload_id, uint32_t offset,
                  const uint8_t* data, uint16_t length);

    /// Finalise the upload. Verifies sha+size and commits to SnapshotStore
    /// on success. Discards the session regardless of outcome.
    SnapshotUploadResult on_end(uint32_t upload_id);

private:
    SnapshotStore& m_store;
    mutable std::mutex m_mu;
    bool m_active = false;
    uint32_t m_id = 0;
    uint32_t m_rev = 0;
    uint64_t m_total_size = 0;
    uint8_t  m_sha[32] = {};
    std::vector<uint8_t> m_buf;  // grows as chunks arrive; sized to total_size on BEGIN
    uint64_t m_received = 0;
};

} // namespace kmp
```

- [ ] **Step 4: Create snapshot_upload.cpp**

```cpp
#include "snapshot_upload.h"
#include "snapshot.h"

#include "picosha2.h"

#include <cstring>

namespace kmp {

static const uint64_t kMaxUploadSize = 512ull * 1024 * 1024;

SnapshotUploadSession::SnapshotUploadSession(SnapshotStore& store)
    : m_store(store) {}

bool SnapshotUploadSession::on_begin(uint32_t upload_id, uint32_t rev,
                                     uint64_t total_size, const uint8_t sha256[32]) {
    if (total_size == 0 || total_size > kMaxUploadSize) return false;
    std::lock_guard<std::mutex> lk(m_mu);
    m_active     = true;
    m_id         = upload_id;
    m_rev        = rev;
    m_total_size = total_size;
    std::memcpy(m_sha, sha256, 32);
    m_buf.assign(static_cast<size_t>(total_size), 0);
    m_received   = 0;
    return true;
}

bool SnapshotUploadSession::on_chunk(uint32_t upload_id, uint32_t offset,
                                     const uint8_t* data, uint16_t length) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_active || upload_id != m_id) return false;
    if (static_cast<uint64_t>(offset) + length > m_total_size) return false;
    std::memcpy(m_buf.data() + offset, data, length);
    m_received += length;
    return true;
}

SnapshotUploadResult SnapshotUploadSession::on_end(uint32_t upload_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_active || upload_id != m_id) return SnapshotUploadResult::NoUpload;

    // Snapshot state in locals and clear session before potentially long
    // sha hashing / store.set so another BEGIN can come in afterwards.
    std::vector<uint8_t> buf = std::move(m_buf);
    uint64_t declared_size   = m_total_size;
    uint64_t received        = m_received;
    uint8_t  declared_sha[32];
    std::memcpy(declared_sha, m_sha, 32);
    m_active = false;
    m_id = 0;
    m_total_size = 0;
    m_received = 0;

    if (received != declared_size) {
        return SnapshotUploadResult::SizeMismatch;
    }

    uint8_t actual_sha[32];
    picosha2::hash256(buf.begin(), buf.end(), actual_sha, actual_sha + 32);
    if (std::memcmp(actual_sha, declared_sha, 32) != 0) {
        return SnapshotUploadResult::ShaMismatch;
    }

    m_store.set(std::move(buf), actual_sha);
    return SnapshotUploadResult::Committed;
}

} // namespace kmp
```

- [ ] **Step 5: Run tests to verify pass**

```bash
cmake --build build_server --target test-snapshot-upload
./build_server/bin/Release/test-snapshot-upload.exe
```

Expected:

```
test_successful_upload OK
test_sha_mismatch_rejects OK
test_size_mismatch_rejects OK
test_new_begin_discards_in_progress OK
ALL PASS
```

- [ ] **Step 6: Commit**

```bash
git add server/core/src/snapshot_upload.h server/core/src/snapshot_upload.cpp tools/test_snapshot_upload.cpp tools/CMakeLists.txt
git commit -m "feat(server): add SnapshotUploadSession for chunk reassembly"
```

---

## Task 5: HTTP sidecar

**Files:**
- Create: `server/core/src/http_sidecar.h`
- Create: `server/core/src/http_sidecar.cpp`
- Modify: `server/core/CMakeLists.txt` (add cpp-httplib include)
- Test: `tools/test_http_sidecar.cpp`

Wraps cpp-httplib. Owns its own listener thread. Serves `GET /snapshot` from a `SnapshotStore&`.

- [ ] **Step 1: Write the failing test**

Create `tools/test_http_sidecar.cpp`:

```cpp
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>

#include "snapshot.h"
#include "http_sidecar.h"
#include "picosha2.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "httplib.h"

using namespace kmp;

static std::vector<uint8_t> make_blob(size_t n) {
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(i & 0xFF);
    return b;
}

static void test_returns_503_when_empty() {
    SnapshotStore store;
    HttpSidecar sidecar(store);
    bool ok = sidecar.start("127.0.0.1", 17891);
    assert(ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    httplib::Client cli("127.0.0.1", 17891);
    auto res = cli.Get("/snapshot");
    assert(res);
    assert(res->status == 503);

    sidecar.stop();
    printf("test_returns_503_when_empty OK\n");
}

static void test_serves_snapshot() {
    SnapshotStore store;
    auto blob = make_blob(4096);
    uint8_t sha[32];
    picosha2::hash256(blob.begin(), blob.end(), sha, sha + 32);
    store.set(blob, sha);

    HttpSidecar sidecar(store);
    assert(sidecar.start("127.0.0.1", 17892));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    httplib::Client cli("127.0.0.1", 17892);
    auto res = cli.Get("/snapshot");
    assert(res);
    assert(res->status == 200);
    assert(res->body.size() == blob.size());
    assert(std::memcmp(res->body.data(), blob.data(), blob.size()) == 0);
    assert(res->get_header_value("X-KMP-Snapshot-Rev") == "1");

    sidecar.stop();
    printf("test_serves_snapshot OK\n");
}

static void test_if_none_match_returns_304() {
    SnapshotStore store;
    auto blob = make_blob(512);
    uint8_t sha[32];
    picosha2::hash256(blob.begin(), blob.end(), sha, sha + 32);
    store.set(blob, sha);

    HttpSidecar sidecar(store);
    assert(sidecar.start("127.0.0.1", 17893));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    httplib::Client cli("127.0.0.1", 17893);
    httplib::Headers hdrs = {{"If-None-Match", "\"1\""}};
    auto res = cli.Get("/snapshot", hdrs);
    assert(res);
    assert(res->status == 304);

    sidecar.stop();
    printf("test_if_none_match_returns_304 OK\n");
}

int main() {
    test_returns_503_when_empty();
    test_serves_snapshot();
    test_if_none_match_returns_304();
    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-http-sidecar test_http_sidecar.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/http_sidecar.cpp)
target_include_directories(test-http-sidecar PRIVATE
    ${CMAKE_SOURCE_DIR}/server/core/src
    ${CMAKE_SOURCE_DIR}/deps/cpp-httplib
    ${CMAKE_SOURCE_DIR}/deps/picosha2)
target_link_libraries(test-http-sidecar PRIVATE kenshi-mp-common ws2_32)
target_compile_features(test-http-sidecar PRIVATE cxx_std_17)
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build_server --target test-http-sidecar
```

Expected: `http_sidecar.h` not found.

- [ ] **Step 3: Create http_sidecar.h**

```cpp
// http_sidecar.h — Small HTTP server running alongside ENet, exposing the
// current save snapshot to joiners on GET /snapshot. Runs on its own thread.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace kmp {

class SnapshotStore;

class HttpSidecar {
public:
    explicit HttpSidecar(SnapshotStore& store);
    ~HttpSidecar();

    HttpSidecar(const HttpSidecar&) = delete;
    HttpSidecar& operator=(const HttpSidecar&) = delete;

    /// Bind to `host:port` and start the listener thread. Returns false if
    /// the port is in use or binding fails.
    bool start(const std::string& host, uint16_t port);

    /// Stop accepting connections and join the thread.
    void stop();

private:
    SnapshotStore& m_store;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace kmp
```

- [ ] **Step 4: Create http_sidecar.cpp**

```cpp
#include "http_sidecar.h"
#include "snapshot.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "httplib.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace kmp {

HttpSidecar::HttpSidecar(SnapshotStore& store)
    : m_store(store), m_server(std::make_unique<httplib::Server>()) {}

HttpSidecar::~HttpSidecar() { stop(); }

bool HttpSidecar::start(const std::string& host, uint16_t port) {
    if (m_running.load()) return false;

    m_server->Get("/snapshot", [this](const httplib::Request& req, httplib::Response& res) {
        std::vector<uint8_t> blob;
        uint32_t rev = 0;
        if (!m_store.get(blob, rev)) {
            res.status = 503;
            res.set_content("no snapshot yet", "text/plain");
            return;
        }

        std::string etag = "\"" + std::to_string(rev) + "\"";
        res.set_header("ETag", etag);
        res.set_header("X-KMP-Snapshot-Rev", std::to_string(rev));

        auto if_none_match = req.get_header_value("If-None-Match");
        if (!if_none_match.empty() && if_none_match == etag) {
            res.status = 304;
            return;
        }

        res.status = 200;
        res.set_content(std::string(reinterpret_cast<const char*>(blob.data()),
                                    blob.size()),
                        "application/zip");
    });

    if (!m_server->bind_to_port(host.c_str(), port)) {
        return false;
    }

    m_running.store(true);
    m_thread = std::thread([this]() {
        m_server->listen_after_bind();
        m_running.store(false);
    });

    return true;
}

void HttpSidecar::stop() {
    if (!m_running.load() && !m_thread.joinable()) return;
    m_server->stop();
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

} // namespace kmp
```

- [ ] **Step 5: Add cpp-httplib include path to server/core/CMakeLists.txt**

Add after the existing `target_include_directories` call:

```cmake
target_include_directories(kenshi-mp-server-core PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/cpp-httplib
    ${CMAKE_SOURCE_DIR}/deps/picosha2
)
```

- [ ] **Step 6: Run tests**

```bash
cmake --build build_server --target test-http-sidecar
./build_server/bin/Release/test-http-sidecar.exe
```

Expected:

```
test_returns_503_when_empty OK
test_serves_snapshot OK
test_if_none_match_returns_304 OK
ALL PASS
```

- [ ] **Step 7: Commit**

```bash
git add server/core/src/http_sidecar.h server/core/src/http_sidecar.cpp server/core/CMakeLists.txt tools/test_http_sidecar.cpp tools/CMakeLists.txt
git commit -m "feat(server): add HttpSidecar exposing GET /snapshot"
```

---

## Task 6: Wire into session handler

**Files:**
- Modify: `server/core/src/session.cpp` (add SNAPSHOT_UPLOAD_* handlers)
- Modify: `server/core/src/session_api.h` (if a public accessor is needed — check during implementation)

The session packet dispatch (search for `handle_connect_request` / packet type switch) gets three new cases. They decode the packet and forward to a `SnapshotUploadSession` instance owned by the session module. On commit / error, an `SnapshotUploadAck` is sent back to the host.

- [ ] **Step 1: Locate the packet dispatcher**

Run:

```bash
grep -n "PacketType::" server/core/src/session.cpp | head -20
```

Find the `switch` that routes by `header.type`. If there's no central dispatch, the packets are handled inline in `session_on_receive` — identify where and plan to add cases there.

- [ ] **Step 2: Add a static SnapshotUploadSession owned by session.cpp**

At the top of `session.cpp`, after the existing `static std::map<...> s_sessions;` declarations (around line 50), add:

```cpp
#include "snapshot.h"
#include "snapshot_upload.h"

// Single active snapshot upload (host is the only sender). The store is
// shared with HttpSidecar — see core.cpp for wiring.
static SnapshotStore*        s_snapshot_store = nullptr;
static std::unique_ptr<SnapshotUploadSession> s_snapshot_session;

void session_bind_snapshot_store(SnapshotStore* store) {
    s_snapshot_store   = store;
    s_snapshot_session.reset(store ? new SnapshotUploadSession(*store) : nullptr);
}
```

Expose the declaration in `session_api.h`:

```cpp
class SnapshotStore;
void session_bind_snapshot_store(SnapshotStore* store);
```

- [ ] **Step 3: Add packet handlers**

Find the packet dispatch switch (or inline if/else chain) in `session.cpp`. Add three cases / branches:

```cpp
case PacketType::SNAPSHOT_UPLOAD_BEGIN: {
    SnapshotUploadBegin pkt;
    if (!unpack(data, length, pkt)) return;
    if (!s_snapshot_session) return;
    bool ok = s_snapshot_session->on_begin(pkt.upload_id, pkt.rev,
                                           pkt.total_size, pkt.sha256);
    if (!ok) {
        SnapshotUploadAck ack;
        ack.upload_id  = pkt.upload_id;
        ack.accepted   = 0;
        ack.error_code = 2;  // size_mismatch (oversize)
        auto buf = pack(ack);
        relay_send_to(peer, buf.data(), buf.size(), true);
    }
    break;
}
case PacketType::SNAPSHOT_UPLOAD_CHUNK: {
    SnapshotUploadChunk hdr;
    const uint8_t* tail = nullptr;
    size_t tail_len = 0;
    if (!unpack_with_tail(data, length, hdr, tail, tail_len)) return;
    if (!s_snapshot_session) return;
    if (tail_len < hdr.length) return;  // truncated
    s_snapshot_session->on_chunk(hdr.upload_id, hdr.offset, tail, hdr.length);
    break;
}
case PacketType::SNAPSHOT_UPLOAD_END: {
    SnapshotUploadEnd pkt;
    if (!unpack(data, length, pkt)) return;
    if (!s_snapshot_session) return;
    SnapshotUploadResult res = s_snapshot_session->on_end(pkt.upload_id);
    SnapshotUploadAck ack;
    ack.upload_id = pkt.upload_id;
    ack.accepted  = (res == SnapshotUploadResult::Committed) ? 1 : 0;
    switch (res) {
        case SnapshotUploadResult::Committed:      ack.error_code = 0; break;
        case SnapshotUploadResult::ShaMismatch:    ack.error_code = 1; break;
        case SnapshotUploadResult::SizeMismatch:   ack.error_code = 2; break;
        case SnapshotUploadResult::NoUpload:       ack.error_code = 4; break;
    }
    auto buf = pack(ack);
    relay_send_to(peer, buf.data(), buf.size(), true);
    if (res == SnapshotUploadResult::Committed) {
        spdlog::info("Snapshot upload committed (rev inc); {} bytes",
                     s_snapshot_store ? s_snapshot_store->rev() : 0u);
    } else {
        spdlog::warn("Snapshot upload failed: err={}", ack.error_code);
    }
    break;
}
```

(If the file uses a different style for dispatch, adapt — the intent is: parse, forward to session, ack on END.)

- [ ] **Step 4: Add snapshot_upload.cpp and snapshot.cpp to server/core/CMakeLists.txt**

Update the `add_library` call:

```cmake
add_library(kenshi-mp-server-core SHARED
    src/core.cpp
    src/events.cpp
    src/session.cpp
    src/world_state.cpp
    src/relay.cpp
    src/server_config.cpp
    src/admin.cpp
    src/spawn.cpp
    src/snapshot.cpp
    src/snapshot_upload.cpp
    src/http_sidecar.cpp
)
```

- [ ] **Step 5: Build server**

```bash
make server 2>&1 | tail -20
```

Expected: clean build, warnings allowed. No errors.

- [ ] **Step 6: Commit**

```bash
git add server/core/src/session.cpp server/core/src/session_api.h server/core/CMakeLists.txt
git commit -m "feat(server): dispatch SNAPSHOT_UPLOAD_* packets into SnapshotUploadSession"
```

---

## Task 7: Wire snapshot store + sidecar into core startup

**Files:**
- Modify: `server/core/src/core.cpp`

Create the `SnapshotStore`, bind it to the session module, start the `HttpSidecar` on `enet_port + 1`. Cleanup on shutdown.

- [ ] **Step 1: Read current startup/shutdown in core.cpp**

```bash
grep -n "kmp_server_start\|kmp_server_stop\|enet_host_create\|session_bind" server/core/src/core.cpp
```

Find the init function (likely `kmp_server_start` exported for FFI) and the corresponding shutdown.

- [ ] **Step 2: Add snapshot store + sidecar lifecycle**

At the top of `core.cpp`, near the other includes, add:

```cpp
#include "snapshot.h"
#include "http_sidecar.h"
#include "session_api.h"
```

Near other static state (search for `static std::unique_ptr<...>` or `static ENetHost*`), add:

```cpp
static std::unique_ptr<SnapshotStore> s_snapshot_store;
static std::unique_ptr<HttpSidecar>   s_http_sidecar;
```

In the server-start function, after the ENet host is created and the port is known (let the exact line number come out of Step 1), add:

```cpp
s_snapshot_store = std::make_unique<SnapshotStore>();
session_bind_snapshot_store(s_snapshot_store.get());

s_http_sidecar = std::make_unique<HttpSidecar>(*s_snapshot_store);
uint16_t http_port = enet_port + 1;
if (!s_http_sidecar->start("0.0.0.0", http_port)) {
    spdlog::error("Failed to bind HTTP sidecar on port {} (in use?)", http_port);
    s_http_sidecar.reset();
    // Server continues — snapshot transfer will be unavailable but relay still works.
} else {
    spdlog::info("HTTP sidecar listening on 0.0.0.0:{}", http_port);
}
```

In the server-stop function, before the ENet teardown:

```cpp
if (s_http_sidecar) { s_http_sidecar->stop(); s_http_sidecar.reset(); }
session_bind_snapshot_store(nullptr);
s_snapshot_store.reset();
```

- [ ] **Step 3: Build server**

```bash
make server 2>&1 | tail -10
```

Expected: clean build.

- [ ] **Step 4: Smoke test — run server, curl sidecar**

Start the server:

```bash
./build_server/bin/Release/kenshi-mp-server.exe
```

Expected in log: `HTTP sidecar listening on 0.0.0.0:7778` (or whatever `port+1` is).

In another terminal:

```bash
curl -v http://127.0.0.1:7778/snapshot
```

Expected: `HTTP/1.1 503` with body `no snapshot yet`.

Stop the server (Ctrl+C). Confirm clean shutdown (no crash, sidecar thread joined).

- [ ] **Step 5: Commit**

```bash
git add server/core/src/core.cpp
git commit -m "feat(server): wire SnapshotStore + HttpSidecar into server lifecycle"
```

---

## Task 8: End-to-end integration test

**Files:**
- Create: `tools/test_snapshot_e2e.cpp`
- Modify: `tools/CMakeLists.txt`

Fake host that connects over ENet, uploads a blob, then an HTTP client that downloads it back. Verifies bytes match.

- [ ] **Step 1: Write the integration test**

Create `tools/test_snapshot_e2e.cpp`:

```cpp
// test_snapshot_e2e.cpp — Upload a blob via ENet SNAPSHOT_UPLOAD_*, then
// download it via HTTP GET /snapshot, verify bytes match.
//
// Requires: server running on 127.0.0.1:7777 (ENet) and 7778 (HTTP).

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

#include <enet/enet.h>

#include "packets.h"
#include "serialization.h"
#include "picosha2.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include "httplib.h"

using namespace kmp;

int main(int argc, char** argv) {
    const char* host_ip    = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t    enet_port  = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 7777;
    uint16_t    http_port  = static_cast<uint16_t>(enet_port + 1);

    if (enet_initialize() != 0) { printf("enet init failed\n"); return 1; }

    ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
    assert(client);

    ENetAddress addr;
    enet_address_set_host(&addr, host_ip);
    addr.port = enet_port;

    ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
    ENetEvent ev;
    bool connected = false;
    for (int i = 0; i < 50 && !connected; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_CONNECT) {
            connected = true;
        }
    }
    if (!connected) { printf("failed to connect\n"); return 1; }

    // Build a 1 MB blob.
    std::vector<uint8_t> blob(1024 * 1024);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i & 0xFF);

    uint8_t sha[32];
    picosha2::hash256(blob.begin(), blob.end(), sha, sha + 32);

    // BEGIN
    SnapshotUploadBegin begin;
    begin.upload_id  = 1;
    begin.rev        = 1;
    begin.total_size = blob.size();
    std::memcpy(begin.sha256, sha, 32);
    auto bbuf = pack(begin);
    ENetPacket* bpkt = enet_packet_create(bbuf.data(), bbuf.size(),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, bpkt);

    // CHUNK (send in 16 KB chunks).
    const uint16_t chunk_sz = 16 * 1024;
    for (size_t off = 0; off < blob.size(); off += chunk_sz) {
        uint16_t len = static_cast<uint16_t>(
            std::min<size_t>(chunk_sz, blob.size() - off));
        SnapshotUploadChunk ch;
        ch.upload_id = 1;
        ch.offset    = static_cast<uint32_t>(off);
        ch.length    = len;
        auto cbuf = pack_with_tail(ch, blob.data() + off, len);
        ENetPacket* cpkt = enet_packet_create(cbuf.data(), cbuf.size(),
                                              ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, 0, cpkt);
    }

    // END
    SnapshotUploadEnd end;
    end.upload_id = 1;
    auto ebuf = pack(end);
    ENetPacket* epkt = enet_packet_create(ebuf.data(), ebuf.size(),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, epkt);
    enet_host_flush(client);

    // Wait for ACK.
    bool acked = false;
    for (int i = 0; i < 100 && !acked; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_RECEIVE) {
            PacketHeader h;
            if (peek_header(ev.packet->data, ev.packet->dataLength, h) &&
                h.type == PacketType::SNAPSHOT_UPLOAD_ACK) {
                SnapshotUploadAck ack;
                if (unpack(ev.packet->data, ev.packet->dataLength, ack)) {
                    assert(ack.accepted == 1);
                    assert(ack.error_code == 0);
                    acked = true;
                }
            }
            enet_packet_destroy(ev.packet);
        }
    }
    assert(acked);
    printf("upload ACKed\n");

    enet_peer_disconnect(peer, 0);
    enet_host_flush(client);
    enet_host_destroy(client);
    enet_deinitialize();

    // Now download via HTTP and compare.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    httplib::Client cli(host_ip, http_port);
    auto res = cli.Get("/snapshot");
    assert(res);
    assert(res->status == 200);
    assert(res->body.size() == blob.size());
    assert(std::memcmp(res->body.data(), blob.data(), blob.size()) == 0);
    printf("HTTP download matches uploaded blob (%zu bytes)\n", blob.size());

    printf("ALL PASS\n");
    return 0;
}
```

Add to `tools/CMakeLists.txt`:

```cmake
add_executable(test-snapshot-e2e test_snapshot_e2e.cpp)
target_include_directories(test-snapshot-e2e PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/cpp-httplib
    ${CMAKE_SOURCE_DIR}/deps/picosha2)
target_link_libraries(test-snapshot-e2e PRIVATE kenshi-mp-common ws2_32)
target_compile_features(test-snapshot-e2e PRIVATE cxx_std_17)
if(ENET_DIR)
    target_include_directories(test-snapshot-e2e PRIVATE ${ENET_DIR}/include)
    target_link_directories(test-snapshot-e2e PRIVATE
        ${ENET_DIR}/lib ${ENET_DIR}/build/Release ${ENET_DIR}/build/Debug)
    target_link_libraries(test-snapshot-e2e PRIVATE enet)
endif()
```

- [ ] **Step 2: Build**

```bash
cmake --build build_server --target test-snapshot-e2e
```

Expected: clean build.

- [ ] **Step 3: Run the end-to-end scenario**

Terminal 1:

```bash
./build_server/bin/Release/kenshi-mp-server.exe
```

Wait for `HTTP sidecar listening on 0.0.0.0:7778`.

Terminal 2:

```bash
./build_server/bin/Release/test-snapshot-e2e.exe
```

Expected:

```
upload ACKed
HTTP download matches uploaded blob (1048576 bytes)
ALL PASS
```

Stop the server (Ctrl+C in terminal 1).

- [ ] **Step 4: Commit**

```bash
git add tools/test_snapshot_e2e.cpp tools/CMakeLists.txt
git commit -m "test: end-to-end snapshot upload + HTTP download"
```

---

## Final verification

All unit tests pass, the end-to-end test passes, the server runs cleanly. At this point Plan A.1 is done — the plumbing is ready for Plan A.2 (host plugin integration) to wire a real Kenshi save into it.

Before marking done, run the whole test suite one more time to catch regressions:

```bash
./build_server/bin/Release/test-snapshot-packets.exe && \
./build_server/bin/Release/test-snapshot-store.exe && \
./build_server/bin/Release/test-snapshot-upload.exe && \
./build_server/bin/Release/test-http-sidecar.exe
```

Expected: all four print `ALL PASS`.

Then the end-to-end as above.

## Self-review notes

Before handing this plan to an executor, I re-read it fresh:

- **Spec coverage**: the spec's "Protocol additions" section (SNAPSHOT_UPLOAD_*, HTTP /snapshot endpoint, X-KMP-Snapshot-Rev / If-None-Match) is covered by Tasks 2-7. The spec's host-side save-trigger and zip producer are explicitly deferred to Plan A.2. The spec's joiner-side download + `importGame` is deferred to Plan A.3. Error handling cases in spec (snapshot upload interrupted, sidecar port in use) are covered by Task 4 (on_begin replaces in-progress), Task 7 (sidecar bind-failure log + continue).
- **Placeholders**: no TBDs. Every code step has concrete code. The only `(If the file uses a different style for dispatch, adapt…)` note is a realistic guardrail for an unknown-shape file, not a placeholder.
- **Type consistency**: `SnapshotStore`, `SnapshotUploadSession`, `SnapshotUploadResult` spelled consistently. Method names (`on_begin`, `on_chunk`, `on_end`, `get`, `set`, `rev`) match across tasks. Packet field names (`upload_id`, `rev`, `total_size`, `sha256`, `offset`, `length`) match between packets.h struct and handlers.
- **Scope**: tight — pure server plumbing. Ships as one milestone even if A.2 and A.3 take months.
