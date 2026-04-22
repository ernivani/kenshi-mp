#include "server_pinger.h"

#include <cstdlib>

namespace kmp {

const ServerPinger::Result ServerPinger::kEmpty;

static const float kConnectTimeoutSec = 2.0f;
static const float kReplyTimeoutSec   = 2.0f;

uint32_t ServerPinger::random_nonce() {
    return (static_cast<uint32_t>(std::rand()) << 16)
         ^ static_cast<uint32_t>(std::rand());
}

ServerPinger::ServerPinger(Deps deps) : m_deps(deps) {}

void ServerPinger::start(const std::string& id,
                         const std::string& address, uint16_t port) {
    Slot& s = m_slots[id];
    s.status      = Status::Connecting;
    s.start_t     = m_deps.now_seconds();
    s.connected_t = 0.0f;
    s.nonce       = random_nonce();
    s.result      = Result();

    bool ok = m_deps.connect(id, address, port);
    if (!ok) {
        s.status = Status::DnsError;
        s.result.status = Status::DnsError;
    }
}

void ServerPinger::tick() {
    float now = m_deps.now_seconds();
    for (std::map<std::string, Slot>::iterator it = m_slots.begin();
         it != m_slots.end(); ++it) {
        Slot& s = it->second;
        if (s.status == Status::Connecting) {
            if (now - s.start_t > kConnectTimeoutSec) {
                s.status = Status::Offline;
                s.result.status = Status::Offline;
                m_deps.disconnect(it->first);
            }
        } else if (s.status == Status::AwaitingReply) {
            if (now - s.connected_t > kReplyTimeoutSec) {
                s.status = Status::NoReply;
                s.result.status = Status::NoReply;
                m_deps.disconnect(it->first);
            }
        }
    }
}

void ServerPinger::on_connected(const std::string& id) {
    std::map<std::string, Slot>::iterator it = m_slots.find(id);
    if (it == m_slots.end()) return;
    Slot& s = it->second;
    if (s.status != Status::Connecting) return;
    s.status      = Status::AwaitingReply;
    s.connected_t = m_deps.now_seconds();
    m_deps.send_request(id, s.nonce);
}

void ServerPinger::on_reply(const std::string& id, uint32_t nonce,
                            const ReplyFields& f) {
    std::map<std::string, Slot>::iterator it = m_slots.find(id);
    if (it == m_slots.end()) return;
    Slot& s = it->second;
    if (s.status != Status::AwaitingReply) return;
    if (nonce != s.nonce) return;

    s.result.player_count      = f.player_count;
    s.result.max_players       = f.max_players;
    s.result.protocol_version  = f.protocol_version;
    s.result.password_required = f.password_required;
    std::memcpy(s.result.description, f.description, sizeof(s.result.description));

    if (f.protocol_version != PROTOCOL_VERSION) {
        s.status = Status::VersionMismatch;
        s.result.status = Status::VersionMismatch;
    } else {
        float now = m_deps.now_seconds();
        s.result.ping_ms = static_cast<uint32_t>((now - s.start_t) * 1000.0f);
        s.status = Status::Success;
        s.result.status = Status::Success;
    }
    m_deps.disconnect(id);
}

ServerPinger::Status::E ServerPinger::status(const std::string& id) const {
    std::map<std::string, Slot>::const_iterator it = m_slots.find(id);
    if (it == m_slots.end()) return Status::Idle;
    return it->second.status;
}

const ServerPinger::Result& ServerPinger::result(const std::string& id) const {
    std::map<std::string, Slot>::const_iterator it = m_slots.find(id);
    if (it == m_slots.end()) return kEmpty;
    return it->second.result;
}

void ServerPinger::clear() {
    for (std::map<std::string, Slot>::iterator it = m_slots.begin();
         it != m_slots.end(); ++it) {
        if (it->second.status == Status::Connecting ||
            it->second.status == Status::AwaitingReply) {
            m_deps.disconnect(it->first);
        }
    }
    m_slots.clear();
}

} // namespace kmp
