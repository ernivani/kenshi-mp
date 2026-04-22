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

struct Recorder { std::vector<std::vector<uint8_t>> sends; };

static SnapshotUploader::Deps make_happy_deps(
        Recorder* rec,
        bool* save_busy,
        bool* zip_started,
        std::vector<uint8_t>* zip_out,
        float* clock) {
    SnapshotUploader::Deps d;
    d.trigger_save = [save_busy](const std::string& slot) -> bool {
        (void)slot; *save_busy = true; return true;
    };
    d.is_save_busy = [save_busy]() -> bool { return *save_busy; };
    d.resolve_slot_path = [](const std::string& slot) -> std::string {
        return "C:/tmp/" + slot;
    };
    d.start_zip = [zip_started](const std::string& path) {
        (void)path; *zip_started = true;
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

    up.tick(0.016f); clock += 0.016f;
    KMP_CHECK(up.state() == SnapshotUploader::State::WAIT_SAVE);

    save_busy = false;
    up.tick(0.016f); clock += 0.016f;
    KMP_CHECK(up.state() == SnapshotUploader::State::ZIP_RUNNING);
    KMP_CHECK(zip_started);

    zip_blob.resize(200 * 1024);
    for (size_t i = 0; i < zip_blob.size(); ++i) {
        zip_blob[i] = static_cast<uint8_t>(i & 0xFF);
    }
    up.tick(0.016f); clock += 0.016f;
    KMP_CHECK(up.state() == SnapshotUploader::State::SEND_CHUNKS);
    KMP_CHECK(rec.sends.size() == 1);
    {
        PacketHeader h;
        KMP_CHECK(peek_header(rec.sends[0].data(), rec.sends[0].size(), h));
        KMP_CHECK(h.type == PacketType::SNAPSHOT_UPLOAD_BEGIN);
    }

    int safety = 1000;
    while (up.state() == SnapshotUploader::State::SEND_CHUNKS && safety-- > 0) {
        up.tick(0.016f); clock += 0.016f;
    }
    KMP_CHECK(safety > 0);
    KMP_CHECK(up.state() == SnapshotUploader::State::AWAIT_ACK);

    {
        PacketHeader h;
        auto& last = rec.sends.back();
        KMP_CHECK(peek_header(last.data(), last.size(), h));
        KMP_CHECK(h.type == PacketType::SNAPSHOT_UPLOAD_END);
    }

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

    SnapshotUploadAck ack;
    {
        SnapshotUploadBegin begin;
        KMP_CHECK(unpack(rec.sends[0].data(), rec.sends[0].size(), begin));
        ack.upload_id = begin.upload_id;
    }
    ack.accepted = 1; ack.error_code = 0;
    up.on_ack(ack);
    KMP_CHECK(up.state() == SnapshotUploader::State::IDLE);

    printf("test_happy_path OK\n");
}

static void test_save_trigger_failure() {
    float clock = 0.0f;
    SnapshotUploader::Deps d;
    d.trigger_save       = [](const std::string&) { return false; };
    d.is_save_busy       = []() { return false; };
    d.resolve_slot_path  = [](const std::string& s) { return "C:/" + s; };
    d.start_zip          = [](const std::string&) {};
    d.poll_zip           = [](std::vector<uint8_t>&) { return false; };
    d.send_reliable      = [](const uint8_t*, size_t) { return true; };
    d.now_seconds        = [&]() { return clock; };

    SnapshotUploader up(d);
    up.start("KMP_Session");
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("save") != std::string::npos);

    printf("test_save_trigger_failure OK\n");
}

static void test_save_wait_timeout() {
    bool save_busy = true;
    float clock = 0.0f;
    SnapshotUploader::Deps d;
    d.trigger_save       = [&](const std::string&) { save_busy = true; return true; };
    d.is_save_busy       = [&]() { return save_busy; };
    d.resolve_slot_path  = [](const std::string& s) { return "C:/" + s; };
    d.start_zip          = [](const std::string&) {};
    d.poll_zip           = [](std::vector<uint8_t>&) { return false; };
    d.send_reliable      = [](const uint8_t*, size_t) { return true; };
    d.now_seconds        = [&]() { return clock; };

    SnapshotUploader up(d);
    up.start("KMP_Session");
    KMP_CHECK(up.state() == SnapshotUploader::State::WAIT_SAVE);

    for (int i = 0; i < 61; ++i) { clock += 1.0f; up.tick(1.0f); }
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("timed out") != std::string::npos);

    printf("test_save_wait_timeout OK\n");
}

static void test_zip_empty_blob_fails() {
    bool save_busy = false;
    bool zip_started = false;
    float clock = 0.0f;

    SnapshotUploader::Deps d;
    d.trigger_save       = [&](const std::string&) { save_busy = false; return true; };
    d.is_save_busy       = [&]() { return save_busy; };
    d.resolve_slot_path  = [](const std::string& s) { return "C:/" + s; };
    d.start_zip          = [&](const std::string&) { zip_started = true; };
    d.poll_zip           = [&](std::vector<uint8_t>& out) -> bool {
        if (!zip_started) return false;
        out.clear(); return true;
    };
    d.send_reliable      = [](const uint8_t*, size_t) { return true; };
    d.now_seconds        = [&]() { return clock; };

    SnapshotUploader up(d);
    up.start("KMP_Session");
    up.tick(0.016f);
    up.tick(0.016f);
    KMP_CHECK(up.state() == SnapshotUploader::State::FAILED);
    KMP_CHECK(up.last_error().find("packaging") != std::string::npos);

    printf("test_zip_empty_blob_fails OK\n");
}

static void test_ack_rejected() {
    Recorder rec;
    bool save_busy = false;
    bool zip_started = false;
    std::vector<uint8_t> blob(1024, 0xAB);
    float clock = 0.0f;

    SnapshotUploader up(make_happy_deps(&rec, &save_busy, &zip_started, &blob, &clock));
    up.start("KMP_Session");
    save_busy = false;  // simulate Kenshi finishing the save
    int safety = 1000;
    while (up.state() != SnapshotUploader::State::AWAIT_ACK && safety-- > 0) {
        up.tick(0.016f); clock += 0.016f;
    }
    KMP_CHECK(safety > 0);

    SnapshotUploadBegin begin;
    KMP_CHECK(unpack(rec.sends[0].data(), rec.sends[0].size(), begin));

    SnapshotUploadAck ack;
    ack.upload_id = begin.upload_id;
    ack.accepted = 0; ack.error_code = 1;
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
    save_busy = false;  // simulate Kenshi finishing the save
    int safety = 1000;
    while (up.state() != SnapshotUploader::State::AWAIT_ACK && safety-- > 0) {
        up.tick(0.016f); clock += 0.016f;
    }
    KMP_CHECK(safety > 0);

    for (int i = 0; i < 31; ++i) { clock += 1.0f; up.tick(1.0f); }
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
