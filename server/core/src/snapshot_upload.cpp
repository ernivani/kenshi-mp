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
