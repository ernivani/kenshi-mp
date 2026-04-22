#include "snapshot_uploader_glue.h"

#include <memory>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "packets.h"
#include "save_trigger.h"
#include "snapshot_uploader.h"
#include "snapshot_zip.h"

// Forward-decl the plugin's ENet send (defined in core/src/client.cpp).
namespace kmp { extern void client_send_reliable(const uint8_t* data, size_t length); }

namespace kmp {

namespace {

// ---------------------------------------------------------------------------
// Win32-based zip background job
// ---------------------------------------------------------------------------
struct ZipJob {
    HANDLE               thread_handle;
    volatile LONG        done;          // 0 = running, 1 = done (interlocked)
    std::vector<uint8_t> result;
    CRITICAL_SECTION     cs;            // guards result

    ZipJob() : thread_handle(NULL), done(0) {
        InitializeCriticalSection(&cs);
    }
    ~ZipJob() {
        DeleteCriticalSection(&cs);
    }
};

struct ZipJobParam {
    std::string abs_path;
    ZipJob*     job;
};

static DWORD WINAPI zip_thread_proc(LPVOID param) {
    ZipJobParam* p = reinterpret_cast<ZipJobParam*>(param);
    std::vector<uint8_t> blob = zip_directory(p->abs_path);
    EnterCriticalSection(&p->job->cs);
    p->job->result = blob;
    LeaveCriticalSection(&p->job->cs);
    InterlockedExchange(&p->job->done, 1L);
    delete p;
    return 0;
}

static std::unique_ptr<SnapshotUploader> s_uploader;
static std::unique_ptr<ZipJob>           s_zip_job;

static float clock_seconds() {
    static LARGE_INTEGER freq;
    static LARGE_INTEGER t0;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        init = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(now.QuadPart - t0.QuadPart) / static_cast<float>(freq.QuadPart);
}

static bool send_reliable_shim(const uint8_t* data, size_t len) {
    client_send_reliable(data, len);
    return true;
}

static void start_zip_bg(const std::string& abs_path) {
    if (!s_zip_job) {
        s_zip_job.reset(new ZipJob());
    } else {
        // Wait for any previous job to finish before restarting.
        if (s_zip_job->thread_handle) {
            WaitForSingleObject(s_zip_job->thread_handle, INFINITE);
            CloseHandle(s_zip_job->thread_handle);
            s_zip_job->thread_handle = NULL;
        }
        InterlockedExchange(&s_zip_job->done, 0L);
        EnterCriticalSection(&s_zip_job->cs);
        s_zip_job->result.clear();
        LeaveCriticalSection(&s_zip_job->cs);
    }

    ZipJobParam* param = new ZipJobParam();
    param->abs_path = abs_path;
    param->job      = s_zip_job.get();

    DWORD tid = 0;
    s_zip_job->thread_handle = CreateThread(NULL, 0, zip_thread_proc, param, 0, &tid);
}

static bool poll_zip_bg(std::vector<uint8_t>& out) {
    if (!s_zip_job) return false;
    if (InterlockedCompareExchange(&s_zip_job->done, 0L, 0L) == 0) return false;
    // Worker is done — reap the thread handle.
    if (s_zip_job->thread_handle) {
        WaitForSingleObject(s_zip_job->thread_handle, INFINITE);
        CloseHandle(s_zip_job->thread_handle);
        s_zip_job->thread_handle = NULL;
    }
    EnterCriticalSection(&s_zip_job->cs);
    out = s_zip_job->result;
    s_zip_job->result.clear();
    LeaveCriticalSection(&s_zip_job->cs);
    InterlockedExchange(&s_zip_job->done, 0L);
    return true;
}

} // namespace

void snapshot_uploader_glue_init() {
    SnapshotUploader::Deps d;
    d.trigger_save       = [](const std::string& slot) { return save_trigger_start(slot); };
    d.is_save_busy       = []() { return save_trigger_is_busy(); };
    d.resolve_slot_path  = [](const std::string& slot) { return save_trigger_resolve_slot_path(slot); };
    d.start_zip          = [](const std::string& path) { start_zip_bg(path); };
    d.poll_zip           = [](std::vector<uint8_t>& out) { return poll_zip_bg(out); };
    d.send_reliable      = [](const uint8_t* data, size_t len) { return send_reliable_shim(data, len); };
    d.now_seconds        = []() { return clock_seconds(); };

    s_uploader.reset(new SnapshotUploader(d));
}

void snapshot_uploader_glue_shutdown() {
    if (s_zip_job && s_zip_job->thread_handle) {
        WaitForSingleObject(s_zip_job->thread_handle, INFINITE);
        CloseHandle(s_zip_job->thread_handle);
        s_zip_job->thread_handle = NULL;
    }
    s_zip_job.reset();
    s_uploader.reset();
}

void snapshot_uploader_glue_start(const std::string& slot) {
    if (!s_uploader) return;
    s_uploader->start(slot);
}

void snapshot_uploader_glue_tick(float dt) {
    if (!s_uploader) return;
    s_uploader->tick(dt);
}

void snapshot_uploader_glue_on_ack(const SnapshotUploadAck& ack) {
    if (!s_uploader) return;
    s_uploader->on_ack(ack);
}

std::string snapshot_uploader_glue_progress_text() {
    if (!s_uploader) return std::string();
    return s_uploader->progress_text();
}

int snapshot_uploader_glue_state_int() {
    if (!s_uploader) return -1;
    return static_cast<int>(s_uploader->state());
}

std::string snapshot_uploader_glue_last_error() {
    if (!s_uploader) return std::string();
    return s_uploader->last_error();
}

} // namespace kmp
