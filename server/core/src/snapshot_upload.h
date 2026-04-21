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
    Committed,
    NoUpload,
    SizeMismatch,
    ShaMismatch,
};

class SnapshotUploadSession {
public:
    explicit SnapshotUploadSession(SnapshotStore& store);

    /// Start (or restart) an upload. Any in-progress upload is discarded.
    /// Returns false if total_size is 0 or exceeds the internal limit (512 MB).
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
    std::vector<uint8_t> m_buf;
    uint64_t m_received = 0;
};

} // namespace kmp
