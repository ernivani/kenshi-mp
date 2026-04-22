#include "joiner_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "server_list.h"

namespace kmp {

static const float kDownloadTimeoutSec = 120.0f;
static const float kLoadTimeoutSec     = 120.0f;
static const float kAcceptTimeoutSec   = 30.0f;

JoinerRuntime::JoinerRuntime(Deps deps)
    : m_deps(deps), m_state(State::Idle),
      m_port(0),
      m_enter_download_t(0.0f), m_enter_load_t(0.0f), m_enter_await_t(0.0f),
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

    m_state = State::Downloading;
    m_enter_download_t = m_deps.now_seconds();
    m_deps.start_download(m_host, m_port, m_zip_path);
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
    case State::LoadTrigger: {
        if (!m_deps.trigger_load(m_deps.resolve_slot_path(""), m_slot)) {
            go_failed("Kenshi refused to load the world");
            break;
        }
        m_state = State::LoadWait;
        m_enter_load_t = m_deps.now_seconds();
        break;
    }
    case State::LoadWait:      tick_load_wait();    break;
    case State::EnetConnect: {
        if (!m_deps.connect_enet(m_host, m_port)) {
            go_failed("Cannot open connection");
            break;
        }
        if (!m_deps.send_connect_request(m_password)) {
            go_failed("Cannot send ConnectRequest");
            break;
        }
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
    if (finished) {
        if (!m_deps.download_succeeded()) {
            go_failed("Download failed");
            return;
        }
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

void JoinerRuntime::tick_load_wait() {
    if (m_deps.is_load_busy()) {
        if (m_deps.now_seconds() - m_enter_load_t > kLoadTimeoutSec) {
            go_failed("Load timed out");
        }
        return;
    }
    m_state = State::EnetConnect;
}

void JoinerRuntime::tick_await_accept() {
    if (m_deps.now_seconds() - m_enter_await_t > kAcceptTimeoutSec) {
        m_deps.disconnect_enet();
        go_failed("Server didn't respond");
    }
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
    if (m_state == State::Downloading && m_bytes_total > 0) {
        float mb_done  = static_cast<float>(m_bytes_done)  / (1024.0f * 1024.0f);
        float mb_total = static_cast<float>(m_bytes_total) / (1024.0f * 1024.0f);
        char buf[64];
        _snprintf(buf, sizeof(buf), "%.1f / %.1f MB", mb_done, mb_total);
        return buf;
    }
    return std::string();
}

} // namespace kmp
