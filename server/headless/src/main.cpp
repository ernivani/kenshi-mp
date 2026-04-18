// server/headless/src/main.cpp — console wrapper around kenshi-mp-server-core.
//
// Loads the core DLL, registers a log callback that writes to stdout,
// starts the server, and waits for Ctrl-C.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "server_api.h"

static std::atomic<bool> s_running{true};
static void on_signal(int) { s_running = false; }

static const char* level_tag(int lvl) {
    switch (lvl) {
    case 0: return "trace";
    case 1: return "debug";
    case 2: return "info";
    case 3: return "warn";
    case 4: return "error";
    case 5: return "crit";
    default: return "?";
    }
}

static void KMP_CALL on_log(int32_t level, uint64_t /*time_ms*/,
                            const char* text, void* /*user*/) {
    std::printf("[%s] %s\n", level_tag(level), text ? text : "");
    std::fflush(stdout);
}

static void KMP_CALL on_event(const kmp_event* e, void* /*user*/) {
    if (!e) return;
    switch (e->type) {
    case KMP_EVT_PLAYER_CONNECTED:
        std::printf("[evt] player %u connected (%s)\n", e->player_id, e->author);
        break;
    case KMP_EVT_PLAYER_DISCONNECTED:
        std::printf("[evt] player %u disconnected (%s)\n", e->player_id, e->author);
        break;
    case KMP_EVT_CHAT_MESSAGE:
        std::printf("[chat] %s: %s\n", e->author, e->text);
        break;
    case KMP_EVT_POSTURE_TRANSITION:
        std::printf("[posture] player %u %02x -> %02x\n",
            e->player_id, e->posture_old, e->posture_new);
        break;
    }
    std::fflush(stdout);
}

int main(int argc, char* argv[]) {
    std::string config_path = (argc > 1) ? argv[1] : "server_config.json";

    kmp_register_log_cb(on_log, nullptr);
    kmp_register_event_cb(on_event, nullptr);

    kmp_server_config cfg;
    kmp_default_config(&cfg);
    kmp_load_config(config_path.c_str(), &cfg);

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    int rc = kmp_server_start(&cfg);
    if (rc != 0) {
        std::fprintf(stderr, "kmp_server_start failed: %d\n", rc);
        return 1;
    }

    while (s_running && kmp_server_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    kmp_server_stop();
    return 0;
}
