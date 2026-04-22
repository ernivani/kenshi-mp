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
    int http = 0;
    SnapshotDownloadResult rc = download_snapshot_blocking(
        job->host, job->port, job->out_path, cb, &job->cancel_flag, &http);
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
    return client_connect(host.c_str(), port);
}

static bool send_connect_request_real(const std::string& password) {
    ConnectRequest req;
    std::strncpy(req.name,  "Player",      MAX_NAME_LENGTH - 1);
    std::strncpy(req.model, "greenlander", MAX_MODEL_LENGTH - 1);
    req.is_host = 0;
    const char* uuid = client_identity_get_uuid();
    std::strncpy(req.client_uuid, uuid ? uuid : "",
                 sizeof(req.client_uuid) - 1);
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
    d.is_load_busy       = []() { return load_trigger_is_busy(); };
    d.connect_enet       = [](const std::string& h, uint16_t p) {
                              return connect_enet_real(h, p); };
    d.send_connect_request = [](const std::string& pw) {
                                return send_connect_request_real(pw); };
    d.disconnect_enet    = []() { disconnect_enet_safe(); };
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
    s_runtime->start(entry);
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
