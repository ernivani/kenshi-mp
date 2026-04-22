// core.cpp — kenshi-mp-server-core.dll implementation.
//
// Owns the ENet host + the service thread. Exposes a small C ABI for the
// Avalonia GUI front-end and the headless wrapper. Fans log lines out to a
// registered callback; fans domain events out via events.h.

#include "server_api.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include "admin.h"
#include "events.h"
#include "protocol.h"
#include "server_config.h"
#include "session_api.h"
#include "spawn.h"
#include "snapshot.h"
#include "http_sidecar.h"

namespace kmp {
    void session_init();
    void session_on_connect(ENetPeer* peer);
    void session_on_disconnect(ENetPeer* peer);
    void session_on_packet(ENetPeer* peer, const uint8_t* data, size_t length);
    void session_check_timeouts();
    void world_state_init();
    void relay_init(ENetHost* host);
    void relay_record_incoming(size_t length);
    uint64_t relay_stat_packets_out();
    uint64_t relay_stat_bytes_out();
    uint64_t relay_stat_packets_in();
    uint64_t relay_stat_bytes_in();
}

namespace {

// ---------------------------------------------------------------------------
// Log fan-out sink
// ---------------------------------------------------------------------------
class CallbackSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    void set_cb(kmp_log_cb cb, void* user) {
        std::lock_guard<std::mutex> lk(base_sink<std::mutex>::mutex_);
        cb_ = cb;
        user_ = user;
    }
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        if (!cb_) return;
        spdlog::memory_buf_t formatted;
        base_sink<std::mutex>::formatter_->format(msg, formatted);
        std::string text(formatted.data(), formatted.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        uint64_t t = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            msg.time.time_since_epoch()).count();
        cb_((int32_t)msg.level, t, text.c_str(), user_);
    }
    void flush_() override {}
private:
    kmp_log_cb cb_ = nullptr;
    void*      user_ = nullptr;
};
static std::shared_ptr<CallbackSink> g_log_sink;

static void ensure_log_sink() {
    if (g_log_sink) return;
    g_log_sink = std::make_shared<CallbackSink>();
    spdlog::default_logger()->sinks().push_back(g_log_sink);
    spdlog::set_level(spdlog::level::debug);
}

// ---------------------------------------------------------------------------
// Worker thread state
// ---------------------------------------------------------------------------
static std::thread          g_worker;
static std::atomic<bool>    g_running{false};
static ENetHost*            g_host = nullptr;
static std::chrono::steady_clock::time_point g_start_time;
static volatile bool        g_run_flag = false;     // watched by admin_request_shutdown
static std::unique_ptr<kmp::SnapshotStore> g_snapshot_store;
static std::unique_ptr<kmp::HttpSidecar>   g_http_sidecar;

static void worker_main(kmp::ServerConfig cfg) {
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = cfg.port;

    g_host = enet_host_create(&address, cfg.max_players, kmp::CHANNEL_COUNT, 0, 0);
    if (!g_host) {
        spdlog::error("Failed to create ENet host on port {}", cfg.port);
        g_running = false;
        return;
    }

    kmp::world_state_init();
    kmp::session_init();

    // Snapshot store + HTTP sidecar. The store is in-RAM; the sidecar binds
    // port+1 on a worker thread and serves GET /snapshot to joiners.
    g_snapshot_store = std::make_unique<kmp::SnapshotStore>();
    kmp::session_bind_snapshot_store(g_snapshot_store.get());
    kmp::session_bind_server_config(&cfg);

    g_http_sidecar = std::make_unique<kmp::HttpSidecar>(*g_snapshot_store);
    uint16_t http_port = static_cast<uint16_t>(cfg.port + 1);
    if (!g_http_sidecar->start("0.0.0.0", http_port)) {
        spdlog::error("Failed to bind HTTP sidecar on port {} (in use?)", http_port);
        g_http_sidecar.reset();
        // Server continues — snapshot transfer unavailable but relay still works.
    } else {
        spdlog::info("HTTP sidecar listening on 0.0.0.0:{}", http_port);
    }

    kmp::relay_init(g_host);
    kmp::admin_set_running_flag(&g_run_flag);
    g_run_flag = true;

    spdlog::info("Server listening on port {}", cfg.port);

    auto last_timeout = std::chrono::steady_clock::now();

    while (g_running && g_run_flag) {
        ENetEvent event;
        while (enet_host_service(g_host, &event, 10) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                spdlog::info("Client connected from {}:{}",
                    event.peer->address.host, event.peer->address.port);
                kmp::session_on_connect(event.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                kmp::relay_record_incoming(event.packet->dataLength);
                kmp::session_on_packet(event.peer, event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                spdlog::info("Client disconnected");
                kmp::session_on_disconnect(event.peer);
                break;
            default: break;
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_timeout).count() >= 5) {
            kmp::session_check_timeouts();
            last_timeout = now;
        }
        kmp::admin_tick();
    }

    spdlog::info("Server stopping...");
    if (g_http_sidecar) { g_http_sidecar->stop(); g_http_sidecar.reset(); }
    kmp::session_bind_snapshot_store(nullptr);
    kmp::session_bind_server_config(nullptr);
    g_snapshot_store.reset();
    enet_host_destroy(g_host);
    g_host = nullptr;
    g_running = false;
    spdlog::info("Server stopped");
}

static bool g_enet_initialized = false;
static std::mutex g_lifecycle_mu;

} // anonymous namespace

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
extern "C" {

KMP_API void KMP_CALL kmp_default_config(kmp_server_config* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->port          = kmp::DEFAULT_PORT;
    out->max_players   = kmp::MAX_PLAYERS;
    out->view_distance = 5000.0f;
    const char* name = "KenshiMP Server";
    std::strncpy(out->server_name, name, sizeof(out->server_name) - 1);
}

KMP_API int32_t KMP_CALL kmp_load_config(const char* path, kmp_server_config* out) {
    if (!out || !path) return -1;
    kmp::ServerConfig cfg = kmp::load_config(path);
    kmp_default_config(out);
    out->port          = cfg.port;
    out->max_players   = cfg.max_players;
    out->view_distance = cfg.view_distance;
    std::strncpy(out->server_name, cfg.server_name.c_str(),
                 sizeof(out->server_name) - 1);
    out->server_name[sizeof(out->server_name) - 1] = '\0';
    return 0;
}

KMP_API int32_t KMP_CALL kmp_save_config(const char* path, const kmp_server_config* cfg) {
    if (!path || !cfg) return -1;
    kmp::ServerConfig c;
    c.port          = cfg->port;
    c.max_players   = cfg->max_players;
    c.view_distance = cfg->view_distance;
    c.server_name   = cfg->server_name;
    return kmp::save_config(path, c) ? 0 : -1;
}

KMP_API int32_t KMP_CALL kmp_server_start(const kmp_server_config* in_cfg) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mu);
    if (g_running) return -1;
    ensure_log_sink();

    if (!g_enet_initialized) {
        if (enet_initialize() != 0) {
            spdlog::error("Failed to initialize ENet");
            return -2;
        }
        g_enet_initialized = true;
    }

    kmp::ServerConfig cfg;
    if (in_cfg) {
        cfg.port          = in_cfg->port;
        cfg.max_players   = in_cfg->max_players;
        cfg.view_distance = in_cfg->view_distance;
        cfg.server_name   = in_cfg->server_name;
    }

    spdlog::info("KenshiMP core starting. Port {}, max {}, name '{}'",
        cfg.port, cfg.max_players, cfg.server_name);

    g_running = true;
    g_start_time = std::chrono::steady_clock::now();
    g_worker = std::thread(worker_main, cfg);
    return 0;
}

KMP_API void KMP_CALL kmp_server_stop(void) {
    std::lock_guard<std::mutex> lk(g_lifecycle_mu);
    if (!g_running) return;
    g_running = false;
    g_run_flag = false;
    if (g_worker.joinable()) g_worker.join();
}

KMP_API int32_t KMP_CALL kmp_server_running(void) {
    return g_running.load() ? 1 : 0;
}

KMP_API void KMP_CALL kmp_register_log_cb(kmp_log_cb cb, void* user) {
    ensure_log_sink();
    if (g_log_sink) g_log_sink->set_cb(cb, user);
}

KMP_API void KMP_CALL kmp_register_event_cb(kmp_event_cb cb, void* user) {
    kmp::events_set_callback(cb, user);
}

KMP_API uint32_t KMP_CALL kmp_get_players(kmp_player_info* out, uint32_t max) {
    if (!out || max == 0) return 0;
    std::vector<kmp::PlayerInfo> v;
    kmp::session_get_players(v);
    uint32_t n = (uint32_t)v.size();
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& s = v[i];
        kmp_player_info& d = out[i];
        std::memset(&d, 0, sizeof(d));
        d.id = s.id;
        d.is_host = s.is_host ? 1 : 0;
        d.ping_ms = s.ping_ms;
        d.idle_ms = s.idle_ms;
        d.x = s.x; d.y = s.y; d.z = s.z;
        d.yaw = s.yaw; d.speed = s.speed;
        d.last_animation_id = s.last_animation_id;
        d.last_posture_flags = s.last_posture_flags;
        std::strncpy(d.name,    s.name.c_str(),    sizeof(d.name) - 1);
        std::strncpy(d.model,   s.model.c_str(),   sizeof(d.model) - 1);
        std::strncpy(d.address, s.address.c_str(), sizeof(d.address) - 1);
    }
    return n;
}

KMP_API void KMP_CALL kmp_get_stats(kmp_stats* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->packets_in  = kmp::relay_stat_packets_in();
    out->packets_out = kmp::relay_stat_packets_out();
    out->bytes_in    = kmp::relay_stat_bytes_in();
    out->bytes_out   = kmp::relay_stat_bytes_out();
    if (g_running)
        out->uptime_seconds = (uint32_t)std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - g_start_time).count();
    std::vector<kmp::PlayerInfo> v;
    kmp::session_get_players(v);
    out->player_count = (uint32_t)v.size();
}

KMP_API int32_t KMP_CALL kmp_kick(uint32_t id, const char* reason) {
    kmp::admin_kick(id, reason ? reason : "");
    return 0;
}

KMP_API int32_t KMP_CALL kmp_broadcast_chat(const char* text) {
    kmp::admin_broadcast_chat(text ? text : "");
    return 0;
}

KMP_API int32_t KMP_CALL kmp_inject_posture(uint32_t id, uint8_t flags, int32_t sticky) {
    kmp::admin_inject_posture(id, flags, sticky != 0);
    return 0;
}

KMP_API void KMP_CALL kmp_clear_sticky_posture(void) {
    kmp::admin_clear_sticky_posture();
}

KMP_API int32_t KMP_CALL kmp_sticky_active(void)  { return kmp::admin_sticky_active() ? 1 : 0; }
KMP_API uint32_t KMP_CALL kmp_sticky_target(void) { return kmp::admin_sticky_target(); }
KMP_API uint8_t KMP_CALL kmp_sticky_flags(void)   { return kmp::admin_sticky_flags(); }

// ---------------------------------------------------------------------------
// Spawn ABI
// ---------------------------------------------------------------------------
KMP_API uint32_t KMP_CALL kmp_spawn_npc(const kmp_npc_spawn_request* req) {
    if (!req) return 0;
    kmp::NPCSpawnRequest r;
    r.name   = req->name;
    r.race   = req->race;
    r.weapon = req->weapon;
    r.armour = req->armour;
    r.x = req->x; r.y = req->y; r.z = req->z; r.yaw = req->yaw;
    r.enable_ai = req->enable_ai != 0;
    return kmp::spawn_npc(r);
}

KMP_API uint32_t KMP_CALL kmp_spawn_building(const kmp_building_spawn_request* req) {
    if (!req) return 0;
    kmp::BuildingSpawnRequest r;
    r.stringID = req->stringID;
    r.x = req->x; r.y = req->y; r.z = req->z;
    r.qw = req->qw; r.qx = req->qx; r.qy = req->qy; r.qz = req->qz;
    r.completed  = req->completed  != 0;
    r.is_foliage = req->is_foliage != 0;
    r.floor = req->floor;
    return kmp::spawn_building(r);
}

KMP_API int32_t KMP_CALL kmp_despawn_npc(uint32_t id)      { return kmp::despawn_npc(id) ? 0 : -1; }
KMP_API int32_t KMP_CALL kmp_despawn_building(uint32_t id) { return kmp::despawn_building(id) ? 0 : -1; }

KMP_API uint32_t KMP_CALL kmp_list_spawned_npcs(kmp_npc_spawned* out, uint32_t max) {
    std::vector<kmp::SpawnedNPC> v;
    kmp::spawned_npcs(v);
    if (!out || max == 0) return (uint32_t)v.size();
    uint32_t n = (uint32_t)v.size(); if (n > max) n = max;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& s = v[i]; kmp_npc_spawned& d = out[i];
        std::memset(&d, 0, sizeof(d));
        d.id = s.id;
        d.x = s.x; d.y = s.y; d.z = s.z; d.yaw = s.yaw;
        std::strncpy(d.name, s.name.c_str(), sizeof(d.name) - 1);
        std::strncpy(d.race, s.race.c_str(), sizeof(d.race) - 1);
    }
    return n;
}

KMP_API uint32_t KMP_CALL kmp_list_building_catalog(kmp_building_catalog_item* out, uint32_t max) {
    std::vector<kmp::BuildingCatalogItem> v;
    kmp::session_building_catalog_snapshot(v);
    if (!out || max == 0) return (uint32_t)v.size();
    uint32_t n = (uint32_t)v.size(); if (n > max) n = max;
    for (uint32_t i = 0; i < n; ++i) {
        std::memset(&out[i], 0, sizeof(out[i]));
        std::strncpy(out[i].stringID, v[i].stringID.c_str(), sizeof(out[i].stringID) - 1);
        std::strncpy(out[i].name,     v[i].name.c_str(),     sizeof(out[i].name)     - 1);
    }
    return n;
}

KMP_API uint32_t KMP_CALL kmp_list_spawned_buildings(kmp_building_spawned* out, uint32_t max) {
    std::vector<kmp::SpawnedBuilding> v;
    kmp::spawned_buildings(v);
    if (!out || max == 0) return (uint32_t)v.size();
    uint32_t n = (uint32_t)v.size(); if (n > max) n = max;
    for (uint32_t i = 0; i < n; ++i) {
        const auto& s = v[i]; kmp_building_spawned& d = out[i];
        std::memset(&d, 0, sizeof(d));
        d.id = s.id;
        d.x = s.x; d.y = s.y; d.z = s.z;
        d.floor = s.floor;
        d.completed  = s.completed  ? 1 : 0;
        d.is_foliage = s.is_foliage ? 1 : 0;
        std::strncpy(d.stringID, s.stringID.c_str(), sizeof(d.stringID) - 1);
    }
    return n;
}

} // extern "C"
