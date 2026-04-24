// snapshot_client.h — WinHTTP wrapper for fetching the host's snapshot
// zip. Blocking (caller runs it on a worker thread).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace kmp {

/// Progress callback — called periodically from inside the blocking
/// download. Safe to write to std::atomic<uint64_t> from here. Must NOT
/// touch MyGUI or other main-thread resources (runs on the worker).
typedef std::function<void(uint64_t bytes_done, uint64_t bytes_total)> SnapshotProgressCb;

enum SnapshotDownloadResult {
    SNAPSHOT_DOWNLOAD_OK              = 0,
    SNAPSHOT_DOWNLOAD_CONNECT_FAILED  = 1,
    SNAPSHOT_DOWNLOAD_HTTP_ERROR      = 2,
    SNAPSHOT_DOWNLOAD_WRITE_FAILED    = 3,
    SNAPSHOT_DOWNLOAD_CANCELLED       = 4,
    SNAPSHOT_DOWNLOAD_UNKNOWN         = 5,
};

/// Synchronously download `http://<host>:<port>/snapshot` to `out_path`.
/// Reports progress via `cb` (may be nullptr). Respects cancellation:
/// if `*cancel_flag` becomes non-zero, aborts ASAP and returns CANCELLED.
/// `http_status_out` receives the status code (or 0 on connect fail).
SnapshotDownloadResult download_snapshot_blocking(
    const std::string& host,
    uint16_t           port,
    const std::string& out_path,
    const SnapshotProgressCb& cb,
    volatile long*     cancel_flag,
    int*               http_status_out);

} // namespace kmp
