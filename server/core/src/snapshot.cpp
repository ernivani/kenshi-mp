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
