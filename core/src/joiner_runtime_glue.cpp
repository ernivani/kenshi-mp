#include "joiner_runtime_glue.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "client_identity.h"
#include "joiner_runtime.h"
#include "kmp_log.h"
#include "load_trigger.h"
#include "packets.h"
#include "serialization.h"
#include "server_list.h"
#include "snapshot_client.h"
#include "snapshot_extract.h"

// Forward-decls from client.cpp.
namespace kmp {
    extern bool client_connect(const char* host, uint16_t port);
    extern void client_disconnect();
    extern void client_send_reliable(const uint8_t* data, size_t length);
    extern void client_poll();
    extern void server_browser_force_close_for_load();
}

namespace kmp {

namespace {

struct DownloadJob {
    HANDLE        thread;
    volatile LONG cancel_flag;
    volatile LONG done;
    volatile LONG succeeded;
    volatile LONG bytes_done_hi, bytes_done_lo;
    volatile LONG bytes_total_hi, bytes_total_lo;
    std::string   host; uint16_t port;
    std::string   out_path;

    DownloadJob() : thread(NULL), cancel_flag(0), done(0), succeeded(0),
                    bytes_done_hi(0), bytes_done_lo(0),
                    bytes_total_hi(0), bytes_total_lo(0), port(0) {}
};

static DWORD WINAPI download_thread_proc(LPVOID param) {
    DownloadJob* job = reinterpret_cast<DownloadJob*>(param);
    SnapshotProgressCb cb = [job](uint64_t done, uint64_t total) {
        InterlockedExchange(&job->bytes_done_hi,  static_cast<LONG>(done  >> 32));
        InterlockedExchange(&job->bytes_done_lo,  static_cast<LONG>(done  & 0xFFFFFFFF));
        InterlockedExchange(&job->bytes_total_hi, static_cast<LONG>(total >> 32));
        InterlockedExchange(&job->bytes_total_lo, static_cast<LONG>(total & 0xFFFFFFFF));
    };

    // Retry loop: the host may not have finished uploading its snapshot
    // when we first ask. On HTTP 503 (no snapshot yet) OR any connect
    // failure, wait 2 s and try again — up to 60 s total.
    // The joiner_runtime's 120s download-timeout wraps this.
    SnapshotDownloadResult rc = SNAPSHOT_DOWNLOAD_UNKNOWN;
    int http = 0;
    const int kMaxAttempts = 30;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (InterlockedCompareExchange(&job->cancel_flag, 0, 0) != 0) {
            rc = SNAPSHOT_DOWNLOAD_CANCELLED; break;
        }
        {
            char buf[192];
            _snprintf(buf, sizeof(buf),
                "[KenshiMP] snapshot_dl: attempt %d/%d host=%s:%u",
                attempt + 1, kMaxAttempts, job->host.c_str(),
                static_cast<unsigned>(job->port));
            KMP_LOG(buf);
        }
        rc = download_snapshot_blocking(
            job->host, job->port, job->out_path, cb, &job->cancel_flag, &http);
        {
            char buf[192];
            _snprintf(buf, sizeof(buf),
                "[KenshiMP] snapshot_dl: attempt %d result rc=%d http=%d",
                attempt + 1, static_cast<int>(rc), http);
            KMP_LOG(buf);
        }
        if (rc == SNAPSHOT_DOWNLOAD_OK) break;
        if (rc == SNAPSHOT_DOWNLOAD_CANCELLED) break;
        // Retryable errors: 503 (host still uploading) OR connect failures
        // (server just started / transient network). Everything else = hard fail.
        bool retry = (rc == SNAPSHOT_DOWNLOAD_HTTP_ERROR && http == 503) ||
                     (rc == SNAPSHOT_DOWNLOAD_CONNECT_FAILED);
        if (!retry) break;
        // Wait 2 s, honoring cancellation.
        for (int i = 0; i < 20; ++i) {
            if (InterlockedCompareExchange(&job->cancel_flag, 0, 0) != 0) {
                rc = SNAPSHOT_DOWNLOAD_CANCELLED;
                break;
            }
            Sleep(100);
        }
        if (rc == SNAPSHOT_DOWNLOAD_CANCELLED) break;
    }
    InterlockedExchange(&job->succeeded, rc == SNAPSHOT_DOWNLOAD_OK ? 1 : 0);
    InterlockedExchange(&job->done, 1);
    return 0;
}

struct ExtractJob {
    HANDLE        thread;
    volatile LONG done;
    volatile LONG succeeded;
    std::string   zip_path;
    std::string   dst_dir;

    ExtractJob() : thread(NULL), done(0), succeeded(0) {}
};

static DWORD WINAPI extract_thread_proc(LPVOID param) {
    ExtractJob* job = reinterpret_cast<ExtractJob*>(param);
    bool ok = extract_zip_to_dir(job->zip_path, job->dst_dir);
    InterlockedExchange(&job->succeeded, ok ? 1 : 0);
    InterlockedExchange(&job->done, 1);
    return 0;
}

static std::unique_ptr<JoinerRuntime> s_runtime;
static std::unique_ptr<DownloadJob>   s_dl_job;
static std::unique_ptr<ExtractJob>    s_ex_job;
static bool                           s_did_snapshot_join = false;

// Async ENet connect. Runs client_connect (which blocks up to 5 s on the
// ENet handshake) on a dedicated thread so it can overlap with the HTTP
// snapshot download. While this thread is live we mustn't let the main
// thread call client_poll/client_send_* against the same ENetHost — the
// enet_host_service call inside client_connect isn't thread-safe. The
// `s_enet_connect_busy` flag gates main-thread ENet access (see
// joiner_runtime_glue_enet_connect_busy()); player_sync_tick skips
// client_poll while it is set.
struct ConnectJob {
    HANDLE        thread;
    std::string   host;
    uint16_t      port;
    volatile LONG done;      // 1 when client_connect returned
    volatile LONG success;   // 1 = connected, 0 = failed
    volatile LONG stop;      // set by main thread to stop the keepalive loop

    ConnectJob() : thread(NULL), port(0), done(0), success(0), stop(0) {}
};
static std::unique_ptr<ConnectJob>   s_co_job;
static volatile LONG                  s_enet_connect_busy = 0;

static DWORD WINAPI connect_thread_proc(LPVOID param) {
    ConnectJob* job = reinterpret_cast<ConnectJob*>(param);
    // Defensive: clear any stale peer before connecting. Mirrors the
    // successful auto-reconnect path (which always runs after a disconnect).
    client_disconnect();
    bool ok = client_connect(job->host.c_str(), job->port);
    InterlockedExchange(&job->success, ok ? 1 : 0);
    InterlockedExchange(&job->done, 1);
    InterlockedExchange(&s_enet_connect_busy, 0);

    // Keep the peer alive until the main thread takes over. Between
    // "connect succeeded" and "player_sync_tick starts polling" there's
    // a multi-second gap where SaveManager::load blocks the main thread.
    // Without client_poll being called during that window ENet keepalives
    // aren't sent/received and the server times the peer out. Run poll
    // here on a 50 ms tick until told to stop.
    while (ok && InterlockedCompareExchange(&job->stop, 0, 0) == 0) {
        client_poll();
        Sleep(50);
    }
    return 0;
}

static void start_async_connect_bg(const std::string& host, uint16_t port) {
    if (s_co_job && s_co_job->thread) {
        WaitForSingleObject(s_co_job->thread, INFINITE);
        CloseHandle(s_co_job->thread);
    }
    s_co_job.reset(new ConnectJob());
    s_co_job->host = host;
    s_co_job->port = port;
    InterlockedExchange(&s_enet_connect_busy, 1);
    s_co_job->thread = CreateThread(NULL, 0, connect_thread_proc,
                                    s_co_job.get(), 0, NULL);
    if (!s_co_job->thread) {
        InterlockedExchange(&s_enet_connect_busy, 0);
        InterlockedExchange(&s_co_job->done, 1);
        InterlockedExchange(&s_co_job->success, 0);
    }
}

static int poll_async_connect_bg() {
    if (!s_co_job) return -1;
    if (InterlockedCompareExchange(&s_co_job->done, 0, 0) == 0) return 0;
    return InterlockedCompareExchange(&s_co_job->success, 0, 0) != 0 ? 1 : -1;
}

static float clock_seconds() {
    static LARGE_INTEGER freq, t0;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        init = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(now.QuadPart - t0.QuadPart) /
           static_cast<float>(freq.QuadPart);
}

static void start_download_bg(const std::string& host, uint16_t port,
                              const std::string& out_path) {
    if (s_dl_job && s_dl_job->thread) {
        WaitForSingleObject(s_dl_job->thread, INFINITE);
        CloseHandle(s_dl_job->thread);
    }
    s_dl_job.reset(new DownloadJob());
    s_dl_job->host = host;
    s_dl_job->port = port;
    s_dl_job->out_path = out_path;
    s_dl_job->thread = CreateThread(NULL, 0, download_thread_proc,
                                    s_dl_job.get(), 0, NULL);
}

static bool poll_download_bg(uint64_t& done, uint64_t& total) {
    if (!s_dl_job) return false;
    uint64_t dhi = static_cast<uint64_t>(
        InterlockedCompareExchange(&s_dl_job->bytes_done_hi, 0, 0));
    uint64_t dlo = static_cast<uint64_t>(static_cast<uint32_t>(
        InterlockedCompareExchange(&s_dl_job->bytes_done_lo, 0, 0)));
    uint64_t thi = static_cast<uint64_t>(
        InterlockedCompareExchange(&s_dl_job->bytes_total_hi, 0, 0));
    uint64_t tlo = static_cast<uint64_t>(static_cast<uint32_t>(
        InterlockedCompareExchange(&s_dl_job->bytes_total_lo, 0, 0)));
    done  = (dhi << 32) | dlo;
    total = (thi << 32) | tlo;
    return InterlockedCompareExchange(&s_dl_job->done, 0, 0) != 0;
}

static void cancel_download_bg() {
    if (!s_dl_job) return;
    InterlockedExchange(&s_dl_job->cancel_flag, 1);
}

static bool download_succeeded_bg() {
    if (!s_dl_job) return false;
    return InterlockedCompareExchange(&s_dl_job->succeeded, 0, 0) != 0;
}

static void start_extract_bg(const std::string& zip_path,
                             const std::string& dst_dir) {
    if (s_ex_job && s_ex_job->thread) {
        WaitForSingleObject(s_ex_job->thread, INFINITE);
        CloseHandle(s_ex_job->thread);
    }
    s_ex_job.reset(new ExtractJob());
    s_ex_job->zip_path = zip_path;
    s_ex_job->dst_dir = dst_dir;
    s_ex_job->thread = CreateThread(NULL, 0, extract_thread_proc,
                                    s_ex_job.get(), 0, NULL);
}

static bool poll_extract_bg(bool& ok) {
    if (!s_ex_job) return false;
    if (InterlockedCompareExchange(&s_ex_job->done, 0, 0) == 0) return false;
    ok = InterlockedCompareExchange(&s_ex_job->succeeded, 0, 0) != 0;
    return true;
}

static bool connect_enet_real(const std::string& host, uint16_t port) {
    // Reset the ENet peer before the first connect. Without this the
    // first ConnectRequest silently gets no reply (server sees stale
    // handshake state?) — only after an explicit disconnect + reconnect
    // (via player_sync auto-reconnect 3s after our timeout) does the
    // server respond. Mimic that reset here so we skip the wasted wait.
    client_disconnect();
    return client_connect(host.c_str(), port);
}

static bool send_connect_request_real(const std::string& password) {
    // IMPORTANT: don't memset(&req, 0, sizeof(req)) here — the struct's
    // default constructor zero-fills AND sets header.version + type.
    // A post-ctor memset wipes those out, producing a packet of type=0
    // that the server silently drops (spent hours chasing this). Just
    // rely on the ctor and set the fields we need.
    ConnectRequest req;
    std::strncpy(req.name,  client_identity_get_name().c_str(),
                 MAX_NAME_LENGTH - 1);
    std::strncpy(req.model, client_identity_get_model().c_str(),
                 MAX_MODEL_LENGTH - 1);
    req.is_host = 0;
    const char* uuid = client_identity_get_uuid();
    if (uuid) std::strncpy(req.client_uuid, uuid, sizeof(req.client_uuid) - 1);
    std::strncpy(req.password, password.c_str(), MAX_PASSWORD_LENGTH - 1);

    std::vector<uint8_t> buf = pack(req);
    client_send_reliable(buf.data(), buf.size());
    return true;
}

static void disconnect_enet_safe() {
    // client_disconnect is a no-op if not connected — safe to call
    // in cancel paths where the pipeline may not have reached EnetConnect.
    client_disconnect();
}

} // namespace

void joiner_runtime_glue_init() {
    JoinerRuntime::Deps d;
    d.start_download     = [](const std::string& h, uint16_t p, const std::string& out) {
                              start_download_bg(h, p, out); };
    d.poll_download      = [](uint64_t& done, uint64_t& total) {
                              return poll_download_bg(done, total); };
    d.cancel_download    = []() { cancel_download_bg(); };
    d.download_succeeded = []() { return download_succeeded_bg(); };
    d.start_extract      = [](const std::string& z, const std::string& dst) {
                              start_extract_bg(z, dst); };
    d.poll_extract       = [](bool& ok) { return poll_extract_bg(ok); };
    d.trigger_load       = [](const std::string& /*loc*/, const std::string& slot) {
                              return load_trigger_start(slot); };
    d.pre_load_cleanup   = []() { server_browser_force_close_for_load(); };
    d.is_load_busy       = []() { return load_trigger_is_busy(); };
    d.connect_enet       = [](const std::string& h, uint16_t p) {
                              return connect_enet_real(h, p); };
    d.send_connect_request = [](const std::string& pw) {
                                return send_connect_request_real(pw); };
    d.disconnect_enet    = []() { disconnect_enet_safe(); };
    d.start_async_connect = [](const std::string& h, uint16_t p) {
                                start_async_connect_bg(h, p); };
    d.poll_async_connect  = []() { return poll_async_connect_bg(); };
    d.now_seconds        = []() { return clock_seconds(); };
    d.resolve_slot_path  = [](const std::string& slot) {
                              return load_trigger_resolve_slot_path(slot); };
    s_runtime.reset(new JoinerRuntime(d));
}

void joiner_runtime_glue_shutdown() {
    if (s_dl_job && s_dl_job->thread) {
        cancel_download_bg();
        WaitForSingleObject(s_dl_job->thread, 2000);
        CloseHandle(s_dl_job->thread);
        s_dl_job->thread = NULL;
    }
    if (s_ex_job && s_ex_job->thread) {
        WaitForSingleObject(s_ex_job->thread, 2000);
        CloseHandle(s_ex_job->thread);
        s_ex_job->thread = NULL;
    }
    s_dl_job.reset();
    s_ex_job.reset();
    s_runtime.reset();
}

void joiner_runtime_glue_start(const ServerEntry& entry) {
    if (!s_runtime) return;
    s_did_snapshot_join = true;
    s_runtime->start(entry);
}

bool joiner_runtime_glue_did_snapshot_join() { return s_did_snapshot_join; }

bool joiner_runtime_glue_enet_connect_busy() {
    return InterlockedCompareExchange(&s_enet_connect_busy, 0, 0) != 0;
}

// Stop the async-connect keepalive loop. Called by player_sync_tick once
// main-thread polling takes over. Idempotent.
void joiner_runtime_glue_stop_keepalive() {
    if (!s_co_job) return;
    InterlockedExchange(&s_co_job->stop, 1);
}

void joiner_runtime_glue_cancel() {
    if (!s_runtime) return;
    s_runtime->cancel();
}

void joiner_runtime_glue_tick(float dt) {
    if (!s_runtime) return;
    s_runtime->tick(dt);
}

void joiner_runtime_glue_on_connect_accept(uint32_t pid) {
    if (!s_runtime) return;
    s_runtime->on_connect_accept(pid);
}

void joiner_runtime_glue_on_connect_reject(const std::string& reason) {
    if (!s_runtime) return;
    s_runtime->on_connect_reject(reason);
}

int joiner_runtime_glue_state_int() {
    if (!s_runtime) return -1;
    return static_cast<int>(s_runtime->state());
}

std::string joiner_runtime_glue_stage_label() {
    if (!s_runtime) return std::string();
    return s_runtime->stage_label();
}

std::string joiner_runtime_glue_progress_text() {
    if (!s_runtime) return std::string();
    return s_runtime->progress_text();
}

std::string joiner_runtime_glue_last_error() {
    if (!s_runtime) return std::string();
    return s_runtime->last_error();
}

} // namespace kmp
