#include "http_sidecar.h"
#include "snapshot.h"

#include "httplib.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace kmp {

HttpSidecar::HttpSidecar(SnapshotStore& store)
    : m_store(store), m_server(std::make_unique<httplib::Server>()) {}

HttpSidecar::~HttpSidecar() { stop(); }

bool HttpSidecar::start(const std::string& host, uint16_t port) {
    if (m_running.load()) return false;

    m_server->Get("/snapshot", [this](const httplib::Request& req, httplib::Response& res) {
        std::vector<uint8_t> blob;
        uint32_t rev = 0;
        if (!m_store.get(blob, rev)) {
            res.status = 503;
            res.set_content("no snapshot yet", "text/plain");
            return;
        }

        std::string etag = "\"" + std::to_string(rev) + "\"";
        res.set_header("ETag", etag);
        res.set_header("X-KMP-Snapshot-Rev", std::to_string(rev));

        auto if_none_match = req.get_header_value("If-None-Match");
        if (!if_none_match.empty() && if_none_match == etag) {
            res.status = 304;
            return;
        }

        res.status = 200;
        res.set_content(std::string(reinterpret_cast<const char*>(blob.data()),
                                    blob.size()),
                        "application/zip");
    });

    if (!m_server->bind_to_port(host.c_str(), port)) {
        return false;
    }

    m_running.store(true);
    m_thread = std::thread([this]() {
        m_server->listen_after_bind();
        m_running.store(false);
    });

    return true;
}

void HttpSidecar::stop() {
    if (!m_running.load() && !m_thread.joinable()) return;
    m_server->stop();
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

} // namespace kmp
