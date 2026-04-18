#include "events.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace kmp {

static std::mutex      s_mu;
static kmp_event_cb    s_cb   = nullptr;
static void*           s_user = nullptr;

static uint64_t wall_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

static void fire(const kmp_event& e) {
    kmp_event_cb cb = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        cb = s_cb;
        user = s_user;
    }
    if (cb) cb(&e, user);
}

static void safe_copy(char* dst, size_t cap, const std::string& src) {
    size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void events_set_callback(kmp_event_cb cb, void* user) {
    std::lock_guard<std::mutex> lk(s_mu);
    s_cb = cb;
    s_user = user;
}

void events_emit_player_connected(uint32_t id, const std::string& name) {
    kmp_event e = {};
    e.type = KMP_EVT_PLAYER_CONNECTED;
    e.player_id = id;
    e.time_ms = wall_ms();
    safe_copy(e.author, sizeof(e.author), name);
    fire(e);
}

void events_emit_player_disconnected(uint32_t id, const std::string& name) {
    kmp_event e = {};
    e.type = KMP_EVT_PLAYER_DISCONNECTED;
    e.player_id = id;
    e.time_ms = wall_ms();
    safe_copy(e.author, sizeof(e.author), name);
    fire(e);
}

void events_emit_chat(uint32_t id, const std::string& author, const std::string& text) {
    kmp_event e = {};
    e.type = KMP_EVT_CHAT_MESSAGE;
    e.player_id = id;
    e.time_ms = wall_ms();
    safe_copy(e.author, sizeof(e.author), author);
    safe_copy(e.text,   sizeof(e.text),   text);
    fire(e);
}

void events_emit_posture(uint32_t id, const std::string& name,
                         uint8_t old_flags, uint8_t new_flags) {
    kmp_event e = {};
    e.type = KMP_EVT_POSTURE_TRANSITION;
    e.player_id = id;
    e.time_ms = wall_ms();
    e.posture_old = old_flags;
    e.posture_new = new_flags;
    safe_copy(e.author, sizeof(e.author), name);
    fire(e);
}

} // namespace kmp
