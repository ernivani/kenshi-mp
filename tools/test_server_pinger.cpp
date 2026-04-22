#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "server_pinger.h"
#include "protocol.h"

using namespace kmp;

namespace {

struct MockEnet {
    std::map<std::string, bool> connect_ok;
    std::vector<std::pair<std::string, uint32_t>> sent_requests;
    std::vector<std::string> disconnects;
};

static ServerPinger::Deps make_deps(MockEnet* mock, float* clock) {
    ServerPinger::Deps d;
    d.connect = [mock](const std::string& id,
                       const std::string& addr, uint16_t port) -> bool {
        (void)addr; (void)port;
        auto it = mock->connect_ok.find(id);
        return (it == mock->connect_ok.end()) ? true : it->second;
    };
    d.send_request = [mock](const std::string& id, uint32_t nonce) {
        mock->sent_requests.push_back(std::make_pair(id, nonce));
    };
    d.disconnect = [mock](const std::string& id) {
        mock->disconnects.push_back(id);
    };
    d.now_seconds = [clock]() { return *clock; };
    return d;
}

static void test_happy_path() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("s1", "1.2.3.4", 7777);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::Connecting);

    pinger.on_connected("s1");
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::AwaitingReply);
    KMP_CHECK(mock.sent_requests.size() == 1);
    KMP_CHECK(mock.sent_requests[0].first == "s1");
    uint32_t nonce = mock.sent_requests[0].second;

    ServerPinger::ReplyFields f;
    f.player_count = 3; f.max_players = 16;
    f.protocol_version = PROTOCOL_VERSION;
    f.password_required = 0;
    std::strncpy(f.description, "Test server", sizeof(f.description));
    clock = 0.042f;
    pinger.on_reply("s1", nonce, f);

    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::Success);
    const ServerPinger::Result& r = pinger.result("s1");
    KMP_CHECK(r.ping_ms > 0 && r.ping_ms < 100);
    KMP_CHECK(r.player_count == 3);
    KMP_CHECK(r.max_players == 16);
    KMP_CHECK(!mock.disconnects.empty() && mock.disconnects.back() == "s1");

    printf("test_happy_path OK\n");
}

static void test_connect_fail_marks_dns_error() {
    MockEnet mock;
    mock.connect_ok["bad"] = false;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("bad", "not.a.host", 7777);
    KMP_CHECK(pinger.status("bad") == ServerPinger::Status::DnsError);

    printf("test_connect_fail_marks_dns_error OK\n");
}

static void test_connect_timeout() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("slow", "1.2.3.4", 7777);
    clock = 2.1f;
    pinger.tick();
    KMP_CHECK(pinger.status("slow") == ServerPinger::Status::Offline);

    printf("test_connect_timeout OK\n");
}

static void test_reply_timeout() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("mute", "1.2.3.4", 7777);
    pinger.on_connected("mute");
    KMP_CHECK(pinger.status("mute") == ServerPinger::Status::AwaitingReply);
    clock = 2.1f;
    pinger.tick();
    KMP_CHECK(pinger.status("mute") == ServerPinger::Status::NoReply);

    printf("test_reply_timeout OK\n");
}

static void test_nonce_mismatch_ignored() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("s1", "1.2.3.4", 7777);
    pinger.on_connected("s1");
    uint32_t real_nonce = mock.sent_requests[0].second;
    uint32_t bad_nonce = real_nonce ^ 0xFFFFFFFF;

    ServerPinger::ReplyFields f; f.player_count = 9;
    f.protocol_version = PROTOCOL_VERSION;
    pinger.on_reply("s1", bad_nonce, f);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::AwaitingReply);

    pinger.on_reply("s1", real_nonce, f);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::Success);

    printf("test_nonce_mismatch_ignored OK\n");
}

static void test_version_mismatch_recorded() {
    MockEnet mock;
    float clock = 0.0f;
    ServerPinger pinger(make_deps(&mock, &clock));

    pinger.start("s1", "1.2.3.4", 7777);
    pinger.on_connected("s1");
    uint32_t nonce = mock.sent_requests[0].second;

    ServerPinger::ReplyFields f;
    f.protocol_version = static_cast<uint8_t>(PROTOCOL_VERSION + 1);
    f.player_count = 0; f.max_players = 0; f.password_required = 0;
    pinger.on_reply("s1", nonce, f);
    KMP_CHECK(pinger.status("s1") == ServerPinger::Status::VersionMismatch);
    KMP_CHECK(pinger.result("s1").protocol_version == PROTOCOL_VERSION + 1);

    printf("test_version_mismatch_recorded OK\n");
}

} // namespace

int main() {
    test_happy_path();
    test_connect_fail_marks_dns_error();
    test_connect_timeout();
    test_reply_timeout();
    test_nonce_mismatch_ignored();
    test_version_mismatch_recorded();
    printf("ALL PASS\n");
    return 0;
}
