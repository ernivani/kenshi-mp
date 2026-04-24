#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "joiner_runtime.h"
#include "server_list.h"

using namespace kmp;

namespace {

struct Mock {
    bool download_started      = false;
    bool download_poll_done    = false;
    bool download_succeeded    = true;
    uint64_t  bytes_done       = 0;
    uint64_t  bytes_total      = 1024 * 1024;
    bool download_cancelled    = false;

    bool extract_started       = false;
    bool extract_poll_done     = false;
    bool extract_succeeded     = true;

    bool load_triggered        = false;
    bool load_trigger_result   = true;
    bool load_still_busy       = true;

    bool connect_called        = false;
    bool connect_result        = true;
    bool send_request_called   = false;
    std::string last_password;

    float now = 0.0f;
    std::string resolved_path = "C:/tmp/KMP";
};

static JoinerRuntime::Deps make_deps(Mock* m) {
    JoinerRuntime::Deps d;
    d.start_download = [m](const std::string&, uint16_t, const std::string&) {
        m->download_started = true;
    };
    d.poll_download = [m](uint64_t& done, uint64_t& total) -> bool {
        done  = m->bytes_done;
        total = m->bytes_total;
        return m->download_poll_done;
    };
    d.cancel_download = [m]() { m->download_cancelled = true; };
    d.download_succeeded = [m]() { return m->download_succeeded; };

    d.start_extract = [m](const std::string&, const std::string&) {
        m->extract_started = true;
    };
    d.poll_extract = [m](bool& ok) -> bool {
        ok = m->extract_succeeded;
        return m->extract_poll_done;
    };

    d.trigger_load = [m](const std::string&, const std::string&) -> bool {
        m->load_triggered = true;
        return m->load_trigger_result;
    };
    d.is_load_busy = [m]() -> bool { return m->load_still_busy; };

    d.connect_enet = [m](const std::string&, uint16_t) -> bool {
        m->connect_called = true;
        return m->connect_result;
    };
    d.send_connect_request = [m](const std::string& pw) -> bool {
        m->send_request_called = true;
        m->last_password = pw;
        return true;
    };
    d.disconnect_enet = []() {};

    d.now_seconds = [m]() { return m->now; };
    d.resolve_slot_path = [m](const std::string&) { return m->resolved_path; };

    return d;
}

static ServerEntry make_entry() {
    ServerEntry e;
    e.id = "abcd1234"; e.name = "Local";
    e.address = "127.0.0.1"; e.port = 7777;
    e.password = "hunter2"; e.last_joined_ms = 0;
    return e;
}

static void drive_until(JoinerRuntime& r, Mock& m, JoinerRuntime::State::E target) {
    int safety = 10000;
    while (r.state() != target && safety-- > 0) {
        r.tick(0.016f);
        m.now += 0.016f;
    }
    KMP_CHECK(safety > 0);
}

static void test_happy_path() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    KMP_CHECK(r.state() == JoinerRuntime::State::Downloading);
    KMP_CHECK(m.download_started);

    m.bytes_done = 512 * 1024;
    r.tick(0.016f);
    m.bytes_done = 1024 * 1024; m.download_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    KMP_CHECK(m.extract_started);

    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadTrigger);
    r.tick(0.016f); m.now += 0.016f;
    KMP_CHECK(r.state() == JoinerRuntime::State::LoadWait);
    KMP_CHECK(m.load_triggered);

    m.now += 5.0f;
    m.load_still_busy = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::EnetConnect);

    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::AwaitAccept);
    KMP_CHECK(m.connect_called);
    KMP_CHECK(m.send_request_called);
    KMP_CHECK(m.last_password == "hunter2");

    r.on_connect_accept(123);
    KMP_CHECK(r.state() == JoinerRuntime::State::Done);
    printf("test_happy_path OK\n");
}

static void test_download_error_times_out() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    for (int i = 0; i < 200; ++i) {
        m.now += 1.0f;
        r.tick(1.0f);
        if (r.state() == JoinerRuntime::State::Failed) break;
    }
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("Download timed out") != std::string::npos);
    printf("test_download_error_times_out OK\n");
}

static void test_download_failed_reports_error() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true;
    m.download_succeeded = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    printf("test_download_failed_reports_error OK\n");
}

static void test_extract_error() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true;
    m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    m.extract_succeeded = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("corrupt") != std::string::npos);
    printf("test_extract_error OK\n");
}

static void test_load_refused() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadTrigger);
    m.load_trigger_result = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    printf("test_load_refused OK\n");
}

static void test_load_timeout() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    for (int i = 0; i < 200; ++i) {
        m.now += 1.0f;
        r.tick(1.0f);
        if (r.state() == JoinerRuntime::State::Failed) break;
    }
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("Load timed out") != std::string::npos);
    printf("test_load_timeout OK\n");
}

static void test_connect_enet_fails() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    m.now += 5.0f; m.load_still_busy = false;
    drive_until(r, m, JoinerRuntime::State::EnetConnect);
    m.connect_result = false;
    r.tick(0.016f);
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    printf("test_connect_enet_fails OK\n");
}

static void test_accept_timeout() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    m.now += 5.0f; m.load_still_busy = false;
    drive_until(r, m, JoinerRuntime::State::AwaitAccept);
    for (int i = 0; i < 40; ++i) {
        m.now += 1.0f; r.tick(1.0f);
        if (r.state() == JoinerRuntime::State::Failed) break;
    }
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("didn't respond") != std::string::npos);
    printf("test_accept_timeout OK\n");
}

static void test_reject_password() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    m.download_poll_done = true; m.bytes_done = 1024;
    drive_until(r, m, JoinerRuntime::State::Extracting);
    m.extract_poll_done = true;
    drive_until(r, m, JoinerRuntime::State::LoadWait);
    m.now += 5.0f; m.load_still_busy = false;
    drive_until(r, m, JoinerRuntime::State::AwaitAccept);
    r.on_connect_reject("wrong password");
    KMP_CHECK(r.state() == JoinerRuntime::State::Failed);
    KMP_CHECK(r.last_error().find("Wrong password") != std::string::npos);
    printf("test_reject_password OK\n");
}

static void test_cancel_during_download() {
    Mock m; JoinerRuntime r(make_deps(&m));
    r.start(make_entry());
    KMP_CHECK(r.state() == JoinerRuntime::State::Downloading);
    r.cancel();
    KMP_CHECK(r.state() == JoinerRuntime::State::Cancelled);
    KMP_CHECK(m.download_cancelled);
    printf("test_cancel_during_download OK\n");
}

} // namespace

int main() {
    test_happy_path();
    test_download_error_times_out();
    test_download_failed_reports_error();
    test_extract_error();
    test_load_refused();
    test_load_timeout();
    test_connect_enet_fails();
    test_accept_timeout();
    test_reject_password();
    test_cancel_during_download();
    printf("ALL PASS\n");
    return 0;
}
