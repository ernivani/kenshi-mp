// server_pinger.h — Batch ENet pinger that resolves SERVER_INFO_REPLY
// per entry. DI interface keeps the real ENet host out of the state
// machine so tests run synchronously without sockets.
//
// State is a struct-wrapped enum so v100 can compile it (no enum class).
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>

#include "protocol.h"

namespace kmp {

class ServerPinger {
public:
    struct Status { enum E {
        Idle,
        Connecting,
        AwaitingReply,
        Success,
        DnsError,
        Offline,
        NoReply,
        VersionMismatch,
    }; };

    struct ReplyFields {
        uint16_t player_count;
        uint16_t max_players;
        uint8_t  protocol_version;
        uint8_t  password_required;
        char     description[128];
        ReplyFields() : player_count(0), max_players(0),
                        protocol_version(0), password_required(0) {
            std::memset(description, 0, sizeof(description));
        }
    };

    struct Result {
        Status::E  status;
        uint32_t   ping_ms;
        uint16_t   player_count;
        uint16_t   max_players;
        uint8_t    protocol_version;
        uint8_t    password_required;
        char       description[128];
        Result() : status(Status::Idle), ping_ms(0), player_count(0),
                   max_players(0), protocol_version(0), password_required(0) {
            std::memset(description, 0, sizeof(description));
        }
    };

    struct Deps {
        std::function<bool(const std::string& id, const std::string& address, uint16_t port)> connect;
        std::function<void(const std::string& id, uint32_t nonce)> send_request;
        std::function<void(const std::string& id)> disconnect;
        std::function<float()> now_seconds;
    };

    explicit ServerPinger(Deps deps);

    void start(const std::string& id, const std::string& address, uint16_t port);
    void tick();
    void on_connected(const std::string& id);
    void on_reply(const std::string& id, uint32_t nonce, const ReplyFields& f);

    Status::E         status(const std::string& id) const;
    const Result&     result(const std::string& id) const;

    void clear();

private:
    struct Slot {
        Status::E status;
        float     start_t;
        float     connected_t;
        uint32_t  nonce;
        Result    result;
    };
    Deps m_deps;
    std::map<std::string, Slot> m_slots;
    static const Result kEmpty;

    static uint32_t random_nonce();
};

} // namespace kmp
