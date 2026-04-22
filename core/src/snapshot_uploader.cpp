#include "snapshot_uploader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "packets.h"
#include "serialization.h"
#include "picosha2.h"

namespace kmp {

namespace {

const float    kSaveTimeoutSec  = 60.0f;
const float    kAckTimeoutSec   = 30.0f;
const uint16_t kChunkSize       = 60 * 1024;
const int      kChunksPerTick   = 2;
const uint64_t kMaxBlobBytes    = 512ull * 1024 * 1024;

static uint32_t rand32() {
    return (static_cast<uint32_t>(std::rand()) << 16) ^ static_cast<uint32_t>(std::rand());
}

} // namespace

SnapshotUploader::SnapshotUploader(Deps deps)
    : m_deps(deps)
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

    if (m_slot_path.empty()) { go_failed("save folder not found"); return; }

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
        m_deps.start_zip(m_slot_path);
        m_state = State::ZIP_RUNNING;
        return;
    }

    case State::ZIP_RUNNING: {
        std::vector<uint8_t> out;
        if (!m_deps.poll_zip(out)) return;
        if (out.empty()) { go_failed("packaging failed"); return; }
        if (out.size() > kMaxBlobBytes) {
            char msg[128];
            _snprintf(msg, sizeof(msg),
                          "save too large (%llu MB > 512 MB)",
                          (unsigned long long)(out.size() / (1024 * 1024)));
            go_failed(msg);
            return;
        }
        m_blob = out;
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

    char msg[64];
    _snprintf(msg, sizeof(msg), "server rejected upload (code=%u)",
                  static_cast<unsigned>(ack.error_code));
    go_failed(msg);
}

SnapshotUploader::State::E SnapshotUploader::state() const { return m_state; }
const std::string& SnapshotUploader::last_error() const { return m_error; }

std::string SnapshotUploader::progress_text() const {
    switch (m_state) {
    case State::IDLE:         return std::string();
    case State::WAIT_SAVE:    return "Hosting: saving world...";
    case State::ZIP_RUNNING:  return "Hosting: packaging world...";
    case State::SEND_CHUNKS: {
        float mb_sent  = static_cast<float>(m_offset) / (1024.0f * 1024.0f);
        float mb_total = static_cast<float>(m_blob.size()) / (1024.0f * 1024.0f);
        char buf[128];
        _snprintf(buf, sizeof(buf),
                      "Hosting: uploading %.1f / %.1f MB", mb_sent, mb_total);
        return buf;
    }
    case State::AWAIT_ACK:    return "Hosting: finalising...";
    case State::FAILED:       return std::string("Hosting: upload failed (") + m_error + ")";
    }
    return std::string();
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

    std::vector<uint8_t> buf = pack(begin);
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

        uint64_t remaining = m_blob.size() - m_offset;
        uint16_t len = static_cast<uint16_t>(
            std::min<uint64_t>(kChunkSize, remaining));

        SnapshotUploadChunk hdr;
        hdr.upload_id = m_upload_id;
        hdr.offset    = static_cast<uint32_t>(m_offset);
        hdr.length    = len;
        std::vector<uint8_t> buf = pack_with_tail(hdr, m_blob.data() + m_offset, len);
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
    std::vector<uint8_t> buf = pack(end);
    if (!m_deps.send_reliable(buf.data(), buf.size())) {
        go_failed("send failed");
        return;
    }
    m_state = State::AWAIT_ACK;
    m_enter_ack_t = m_deps.now_seconds();
}

} // namespace kmp
