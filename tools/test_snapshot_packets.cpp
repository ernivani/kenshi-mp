#include "test_check.h"
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
    KMP_CHECK(ok);
    KMP_CHECK(got.upload_id  == 0x12345678);
    KMP_CHECK(got.rev        == 7);
    KMP_CHECK(got.total_size == 1024 * 1024);
    for (int i = 0; i < 32; ++i) KMP_CHECK(got.sha256[i] == static_cast<uint8_t>(i * 3));
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
    KMP_CHECK(ok);
    KMP_CHECK(got_hdr.upload_id == 99);
    KMP_CHECK(got_hdr.offset    == 65536);
    KMP_CHECK(got_hdr.length    == data.size());
    KMP_CHECK(tail_len == data.size());
    KMP_CHECK(std::memcmp(tail, data.data(), data.size()) == 0);
    printf("test_upload_chunk_roundtrip OK\n");
}

static void test_upload_end_roundtrip() {
    SnapshotUploadEnd orig;
    orig.upload_id = 42;
    auto buf = pack(orig);
    SnapshotUploadEnd got;
    bool ok = unpack(buf.data(), buf.size(), got);
    KMP_CHECK(ok);
    KMP_CHECK(got.upload_id == 42);
    printf("test_upload_end_roundtrip OK\n");
}

int main() {
    test_upload_begin_roundtrip();
    test_upload_chunk_roundtrip();
    test_upload_end_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
