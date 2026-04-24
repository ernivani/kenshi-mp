#include "joiner_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "kmp_log.h"
#include "server_list.h"

namespace kmp {

static const char* state_name(int s) {
    switch (s) {
    case 0: return "Idle";
    case 1: return "Downloading";
    case 2: return "Extracting";
    case 3: return "LoadTrigger";
    case 4: return "LoadWait";
    case 5: return "EnetConnect";
    case 6: return "AwaitAccept";
    case 7: return "Done";
    case 8: return "Cancelled";
    case 9: return "Failed";
    default: return "?";
    }
}

#define KMP_JR_LOG_STATE(label) do { \
    char _buf[128]; \
    _snprintf(_buf, sizeof(_buf), "[KenshiMP] joiner_runtime: %s state=%s", \
              label, state_name(static_cast<int>(m_state))); \
    KMP_LOG(_buf); \
} while(0)

static const float kDownloadTimeoutSec = 120.0f;
static const float kLoadTimeoutSec     = 120.0f;
// Short per-attempt timeout. The first ConnectRequest frequently gets
// no CONNECT_ACCEPT reply; we retry (disconnect + reconnect + resend)
// until either accepted or we blow past the total budget.
static const float kAcceptTimeoutSec   = 3.0f;
static const float kConnectTotalBudget = 15.0f;
static const int   kMaxConnectAttempts = 5;

JoinerRuntime::JoinerRuntime(Deps deps)
    : m_deps(deps), m_state(State::Idle),
      m_port(0),
      m_enter_download_t(0.0f), m_enter_load_trigger_t(0.0f),
      m_enter_load_t(0.0f), m_load_finished_t(0.0f), m_enter_await_t(0.0f),
      m_first_connect_t(0.0f), m_connect_attempts(0),
      m_pre_load_hidden(false),
      m_bytes_done(0), m_bytes_total(0) {}

void JoinerRuntime::start(const ServerEntry& entry) {
    m_host = entry.address;
    m_port = entry.port;
    m_password = entry.password;
    m_slot = std::string("KMP_") + entry.id;
    m_slot_dir = m_deps.resolve_slot_path(m_slot);
    m_zip_path = m_slot_dir + ".zip";
    m_error.clear();
    m_bytes_done = 0;
    m_bytes_total = 0;
    m_pre_load_hidden = false;
    m_first_connect_t = 0.0f;
    m_connect_attempts = 0;

    m_state = State::Downloading;
    m_enter_download_t = m_deps.now_seconds();
    {
        char buf[256];
        _snprintf(buf, sizeof(buf),
            "[KenshiMP] joiner_runtime: START host=%s:%u slot=%s zip=%s",
            m_host.c_str(), static_cast<unsigned>(m_port),
            m_slot.c_str(), m_zip_path.c_str());
        KMP_LOG(buf);
    }
    // We do NOT start ENet connect here. Tried it both synchronously and
    // on a background thread — both fail: the peer ends up in some state
    // where the server's CONNECT_ACCEPT reply is never received. Only
    // after an explicit disconnect + fresh connect (like the legacy
    // auto-reconnect path does) does the server respond. So we defer
    // the connect to the EnetConnect state, which does disconnect+connect
    // right before sending ConnectRequest.
    // HTTP sidecar listens on ENet port + 1 (see server/core/src/core.cpp).
    m_deps.start_download(m_host, static_cast<uint16_t>(m_port + 1), m_zip_path);
}

void JoinerRuntime::cancel() {
    switch (m_state) {
    case State::Downloading:
        m_deps.cancel_download();
        m_state = State::Cancelled;
        break;
    case State::Extracting:
    case State::LoadTrigger:
    case State::LoadWait:
    case State::EnetConnect:
    case State::AwaitAccept:
        m_deps.disconnect_enet();
        m_state = State::Cancelled;
        break;
    default: break;
    }
}

void JoinerRuntime::tick(float /*dt*/) {
    switch (m_state) {
    case State::Downloading:   tick_download();     break;
    case State::Extracting:    tick_extract();      break;
    case State::LoadTrigger:   tick_load_trigger(); break;
    case State::LoadWait:      tick_load_wait();    break;
    case State::EnetConnect: {
        if (m_first_connect_t == 0.0f) m_first_connect_t = m_deps.now_seconds();
        ++m_connect_attempts;
        char buf[160];
        _snprintf(buf, sizeof(buf),
            "[KenshiMP] joiner_runtime: EnetConnect host=%s:%u (attempt %d/%d)",
            m_host.c_str(), static_cast<unsigned>(m_port),
            m_connect_attempts, kMaxConnectAttempts);
        KMP_LOG(buf);
        m_deps.disconnect_enet();
        if (!m_deps.connect_enet(m_host, m_port)) {
            go_failed("Cannot open connection");
            break;
        }
        if (!m_deps.send_connect_request(m_password)) {
            go_failed("Cannot send ConnectRequest");
            break;
        }
        KMP_LOG("[KenshiMP] joiner_runtime: sent ConnectRequest, awaiting accept");
        m_state = State::AwaitAccept;
        m_enter_await_t = m_deps.now_seconds();
        break;
    }
    case State::AwaitAccept:   tick_await_accept(); break;
    default: break;
    }
}

void JoinerRuntime::tick_download() {
    uint64_t done = 0, total = 0;
    bool finished = m_deps.poll_download(done, total);
    m_bytes_done = done;
    m_bytes_total = total;
    // Log progress periodically.
    static int s_dl_log_counter = 0;
    if ((s_dl_log_counter++ % 120) == 0) {
        char buf[128];
        _snprintf(buf, sizeof(buf),
            "[KenshiMP] joiner_runtime: download tick finished=%d bytes=%llu/%llu",
            finished ? 1 : 0,
            static_cast<unsigned long long>(done),
            static_cast<unsigned long long>(total));
        KMP_LOG(buf);
    }
    if (finished) {
        if (!m_deps.download_succeeded()) {
            go_failed("Download failed");
            return;
        }
        KMP_LOG("[KenshiMP] joiner_runtime: download OK, starting extract");
        m_deps.start_extract(m_zip_path, m_slot_dir);
        m_state = State::Extracting;
        return;
    }
    if (m_deps.now_seconds() - m_enter_download_t > kDownloadTimeoutSec) {
        m_deps.cancel_download();
        go_failed("Download timed out");
    }
}

void JoinerRuntime::tick_extract() {
    bool ok = false;
    if (!m_deps.poll_extract(ok)) return;
    if (!ok) { go_failed("Extracted world is corrupt"); return; }
    m_state = State::LoadTrigger;
}

void JoinerRuntime::tick_load_trigger() {
    // Two-phase: first hide our MyGUI widgets, then wait ~300 ms so
    // TitleScreen renders a clean frame, THEN call SaveManager::loadGame.
    // Calling loadGame synchronously from inside TitleScreen::update's tick
    // while our ConnectingWindow is live crashes the game in kenshi_x64.exe
    // (NULL write at +0x49FAD6) — presumably because loadGame tears down
    // state that the render pass is still iterating.
    if (!m_pre_load_hidden) {
        if (m_deps.pre_load_cleanup) m_deps.pre_load_cleanup();
        m_pre_load_hidden = true;
        m_enter_load_trigger_t = m_deps.now_seconds();
        KMP_LOG("[KenshiMP] joiner_runtime: UI hidden, deferring loadGame");
        return;
    }
    if (m_deps.now_seconds() - m_enter_load_trigger_t < 0.3f) return;
    KMP_LOG("[KenshiMP] joiner_runtime: calling trigger_load now");
    if (!m_deps.trigger_load(m_deps.resolve_slot_path(""), m_slot)) {
        go_failed("Kenshi refused to load the world");
        return;
    }
    m_state = State::LoadWait;
    m_enter_load_t = m_deps.now_seconds();
}

void JoinerRuntime::tick_load_wait() {
    if (m_deps.is_load_busy()) {
        if (m_deps.now_seconds() - m_enter_load_t > kLoadTimeoutSec) {
            go_failed("Load timed out");
        }
        return;
    }
    KMP_LOG("[KenshiMP] joiner_runtime: LoadWait -> EnetConnect");
    m_state = State::EnetConnect;
}

void JoinerRuntime::tick_await_accept() {
    float now = m_deps.now_seconds();
    if (now - m_enter_await_t <= kAcceptTimeoutSec) return;

    // Per-attempt timeout expired. Retry (go back to EnetConnect) unless
    // we've blown the total budget or exhausted the attempt cap.
    bool over_budget = (now - m_first_connect_t) > kConnectTotalBudget;
    if (m_connect_attempts >= kMaxConnectAttempts || over_budget) {
        m_deps.disconnect_enet();
        go_failed("Server didn't respond");
        return;
    }
    char buf[160];
    _snprintf(buf, sizeof(buf),
        "[KenshiMP] joiner_runtime: accept timeout after %.1fs — retrying "
        "(attempt %d done, elapsed %.1fs)",
        kAcceptTimeoutSec, m_connect_attempts, now - m_first_connect_t);
    KMP_LOG(buf);
    m_state = State::EnetConnect;
}

void JoinerRuntime::on_connect_accept(uint32_t /*player_id*/) {
    if (m_state != State::AwaitAccept) return;
    m_state = State::Done;
}

void JoinerRuntime::on_connect_reject(const std::string& reason) {
    if (m_state != State::AwaitAccept) return;
    if (reason.find("wrong password") != std::string::npos) {
        go_failed("Wrong password");
    } else {
        go_failed(std::string("Rejected: ") + reason);
    }
    m_deps.disconnect_enet();
}

void JoinerRuntime::go_failed(const std::string& msg) {
    m_error = msg;
    m_state = State::Failed;
    KMP_LOG(std::string("[KenshiMP] joiner_runtime: FAILED — ") + msg);
}

JoinerRuntime::State::E JoinerRuntime::state() const { return m_state; }
const std::string& JoinerRuntime::last_error() const { return m_error; }

std::string JoinerRuntime::stage_label() const {
    switch (m_state) {
    case State::Downloading: return "Downloading world";
    case State::Extracting:  return "Extracting";
    case State::LoadTrigger:
    case State::LoadWait:    return "Loading world";
    case State::EnetConnect:
    case State::AwaitAccept: return "Connecting";
    default:                 return std::string();
    }
}

std::string JoinerRuntime::progress_text() const {
    if (m_state == State::Downloading) {
        float elapsed = m_deps.now_seconds() - m_enter_download_t;
        if (elapsed < 0.0f) elapsed = 0.0f;
        if (m_bytes_total > 0 && m_bytes_done > 0) {
            float mb_done  = static_cast<float>(m_bytes_done)  / (1024.0f * 1024.0f);
            float mb_total = static_cast<float>(m_bytes_total) / (1024.0f * 1024.0f);
            float pct      = 100.0f * static_cast<float>(m_bytes_done) /
                             static_cast<float>(m_bytes_total);
            float rate     = static_cast<float>(m_bytes_done) /
                             (elapsed > 0.1f ? elapsed : 0.1f); // bytes/sec
            float eta_s    = rate > 1.0f
                ? static_cast<float>(m_bytes_total - m_bytes_done) / rate
                : 0.0f;
            float mbps     = rate / (1024.0f * 1024.0f);
            char buf[128];
            _snprintf(buf, sizeof(buf),
                "%.0f%%  %.1f / %.1f MB  %.1f MB/s  ETA %ds",
                pct, mb_done, mb_total, mbps, static_cast<int>(eta_s));
            return buf;
        }
        // No bytes yet — show "waiting for host" after first second to
        // distinguish from a fast start.
        if (elapsed > 1.0f) {
            char buf[64];
            _snprintf(buf, sizeof(buf),
                "waiting for host... (%ds)", static_cast<int>(elapsed));
            return buf;
        }
    }
    return std::string();
}

} // namespace kmp
