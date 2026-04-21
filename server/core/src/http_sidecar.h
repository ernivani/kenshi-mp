// http_sidecar.h — Small HTTP server running alongside ENet, exposing the
// current save snapshot to joiners on GET /snapshot. Runs on its own thread.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace kmp {

class SnapshotStore;

class HttpSidecar {
public:
    explicit HttpSidecar(SnapshotStore& store);
    ~HttpSidecar();

    HttpSidecar(const HttpSidecar&) = delete;
    HttpSidecar& operator=(const HttpSidecar&) = delete;

    /// Bind to `host:port` and start the listener thread. Returns false if
    /// the port is in use or binding fails.
    bool start(const std::string& host, uint16_t port);

    /// Stop accepting connections and join the thread.
    void stop();

private:
    SnapshotStore& m_store;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace kmp
