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
