// joiner_runtime.h — State machine that drives the full joiner pipeline
// (download → extract → load → connect). All external calls are DI'd as
// std::function so tests run synchronously without real threads, HTTP,
// Kenshi, or ENet.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace kmp {

struct ServerEntry;

class JoinerRuntime {
public:
    struct State { enum E {
        Idle,
        Downloading,
        Extracting,
        LoadTrigger,
        LoadWait,
        EnetConnect,
        AwaitAccept,
        Done,
        Cancelled,
        Failed,
    }; };

    struct Deps {
        std::function<void(const std::string& host, uint16_t port, const std::string& out_path)> start_download;
        std::function<bool(uint64_t& bytes_done, uint64_t& bytes_total)>                          poll_download;
        std::function<void()>                                                                     cancel_download;
        std::function<bool()>                                                                     download_succeeded;

        std::function<void(const std::string& zip_path, const std::string& dst_dir)>              start_extract;
        std::function<bool(bool& ok)>                                                             poll_extract;

        std::function<bool(const std::string& location, const std::string& slot)>                 trigger_load;
        std::function<bool()>                                                                     is_load_busy;
        // Optional: called once on the render thread before trigger_load, so
        // our menu widgets (browser, connecting modal) are hidden/detached
        // before SaveManager::loadGame tears down TitleScreen. Without this
        // the game crashes in kenshi_x64.exe+0x49FAD6 (NULL write).
        std::function<void()>                                                                     pre_load_cleanup;

        // Legacy sync connect (kept for tests). Prefer async pair below.
        std::function<bool(const std::string& host, uint16_t port)>                               connect_enet;
        std::function<bool(const std::string& password)>                                          send_connect_request;
        std::function<void()>                                                                     disconnect_enet;
        // Async ENet connect. Fired at start() in parallel with download,
        // so the peer is handshaken by the time the save finishes loading.
        // start(host, port) kicks off a background thread; poll() returns
        // 0=pending, 1=connected, -1=failed. Optional — if unset, the
        // runtime falls back to the sync connect_enet.
        std::function<void(const std::string& host, uint16_t port)>                               start_async_connect;
        std::function<int()>                                                                      poll_async_connect;

        std::function<float()>                                                                    now_seconds;
        std::function<std::string(const std::string& slot)>                                       resolve_slot_path;
    };

    explicit JoinerRuntime(Deps deps);

    void start(const ServerEntry& entry);
    void cancel();
    void tick(float dt);
    void on_connect_accept(uint32_t player_id);
    void on_connect_reject(const std::string& reason);

    State::E           state() const;
    const std::string& last_error() const;
    std::string        progress_text() const;
    std::string        stage_label() const;

private:
    Deps m_deps;
    State::E m_state;
    std::string m_host;
    uint16_t    m_port;
    std::string m_password;
    std::string m_slot;
    std::string m_zip_path;
    std::string m_slot_dir;

    float m_enter_download_t;
    float m_enter_load_trigger_t;
    float m_enter_load_t;
    float m_load_finished_t;   // when SaveFileSystem went idle
    float m_enter_await_t;
    float m_first_connect_t;   // when we first entered EnetConnect
    int   m_connect_attempts;  // count of ConnectRequest retries
    bool  m_pre_load_hidden;

    uint64_t m_bytes_done;
    uint64_t m_bytes_total;

    std::string m_error;

    void go_failed(const std::string& msg);
    void tick_download();
    void tick_extract();
    void tick_load_trigger();
    void tick_load_wait();
    void tick_await_accept();
};

} // namespace kmp
