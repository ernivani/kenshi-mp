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
    assert(!store.has_snapshot());

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
    auto tmp = make_blob(2048, 0);
    sess.on_chunk(10, 0, tmp.data(), 2048);

    sess.on_begin(11, 2, 1024, sha);

    bool ok = sess.on_chunk(10, 2048, tmp.data(), 2048);
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
