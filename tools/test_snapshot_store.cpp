#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include "snapshot.h"

using namespace kmp;

static void test_empty_store_has_no_snapshot() {
    SnapshotStore store;
    std::vector<uint8_t> blob;
    uint32_t rev = 0;
    KMP_CHECK(!store.get(blob, rev));
    KMP_CHECK(rev == 0);
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
    KMP_CHECK(store.get(got_blob, got_rev));
    KMP_CHECK(got_rev == 1);
    KMP_CHECK(got_blob.size() == 5);
    KMP_CHECK(got_blob[0] == 1);
    KMP_CHECK(got_blob[4] == 5);

    uint8_t got_sha[32] = {};
    store.get_sha(got_sha);
    for (int i = 0; i < 32; ++i) KMP_CHECK(got_sha[i] == static_cast<uint8_t>(i));

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
    KMP_CHECK(store.get(blob, rev));
    KMP_CHECK(rev == 3);
    KMP_CHECK(blob.size() == 1 && blob[0] == 7);
    printf("test_rev_increments_on_set OK\n");
}

int main() {
    test_empty_store_has_no_snapshot();
    test_set_and_get_snapshot();
    test_rev_increments_on_set();
    printf("ALL PASS\n");
    return 0;
}
