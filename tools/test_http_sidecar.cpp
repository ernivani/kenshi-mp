#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

#include "snapshot.h"
#include "http_sidecar.h"
#include "picosha2.h"

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
