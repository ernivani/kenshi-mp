// server_api.h — C ABI for kenshi-mp-server-core.dll
//
// Consumed by: the Avalonia GUI (P/Invoke), the headless C++ wrapper,
// automated tests. All strings are UTF-8 null-terminated const char*.
// Callbacks run on the server worker thread; consumers must marshal to UI.

#ifndef KMP_SERVER_API_H
#define KMP_SERVER_API_H

#include <stdint.h>

#if defined(_WIN32)
  #ifdef KMP_SERVER_CORE_EXPORTS
    #define KMP_API __declspec(dllexport)
  #else
    #define KMP_API __declspec(dllimport)
  #endif
  #define KMP_CALL __cdecl
#else
  #define KMP_API
  #define KMP_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// POD types for interop
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

typedef struct {
    uint16_t port;
    uint32_t max_players;
    char     server_name[128];
    float    view_distance;
} kmp_server_config;

typedef struct {
    uint32_t id;
    uint8_t  is_host;
    uint32_t ping_ms;
    uint32_t idle_ms;
    float    x, y, z;
    float    yaw;
    float    speed;
    uint32_t last_animation_id;
    uint8_t  last_posture_flags;
    char     name[64];
    char     model[64];
    char     address[64];   // "ip:port"
} kmp_player_info;

typedef struct {
    uint64_t packets_in;
    uint64_t packets_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint32_t uptime_seconds;
    uint32_t player_count;
} kmp_stats;

typedef struct {
    char     name[64];
    char     race[64];
    char     weapon[64];
    char     armour[64];
    float    x, y, z;
    float    yaw;
    uint8_t  enable_ai;   // 0 = no AI (default), 1 = let faction AI run
} kmp_npc_spawn_request;

typedef struct {
    uint32_t id;
    char     name[64];
    char     race[64];
    float    x, y, z;
    float    yaw;
} kmp_npc_spawned;

typedef struct {
    char     stringID[64];
    float    x, y, z;
    float    qw, qx, qy, qz;
    uint8_t  completed;
    uint8_t  is_foliage;
    int16_t  floor;
} kmp_building_spawn_request;

typedef struct {
    uint32_t id;
    char     stringID[64];
    float    x, y, z;
    int16_t  floor;
    uint8_t  completed;
    uint8_t  is_foliage;
} kmp_building_spawned;

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Event enum — events flow over a single event callback.
// ---------------------------------------------------------------------------
typedef enum {
    KMP_EVT_PLAYER_CONNECTED     = 1,
    KMP_EVT_PLAYER_DISCONNECTED  = 2,
    KMP_EVT_CHAT_MESSAGE         = 3,
    KMP_EVT_POSTURE_TRANSITION   = 4,
} kmp_event_type;

typedef struct {
    int32_t       type;         // kmp_event_type
    uint32_t      player_id;
    uint64_t      time_ms;      // wall-clock ms since epoch
    uint8_t       posture_old;  // POSTURE_* low 8 bits
    uint8_t       posture_new;
    char          author[64];   // for chat (name) / posture (player name)
    char          text[256];    // chat body, empty otherwise
} kmp_event;

// Log levels mirror spdlog: trace=0 debug=1 info=2 warn=3 err=4 critical=5
typedef void (KMP_CALL *kmp_log_cb)(int32_t level, uint64_t time_ms,
                                    const char* text, void* user);
typedef void (KMP_CALL *kmp_event_cb)(const kmp_event* evt, void* user);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Start the server. Returns 0 on success, non-zero on failure.
// Runs ENet + session logic on a background thread owned by the DLL.
KMP_API int32_t KMP_CALL kmp_server_start(const kmp_server_config* cfg);

// Request graceful shutdown. Blocks until the worker thread joins.
KMP_API void    KMP_CALL kmp_server_stop(void);

KMP_API int32_t KMP_CALL kmp_server_running(void);

// ---------------------------------------------------------------------------
// Callbacks (register once before/after start; safe to call either side)
// ---------------------------------------------------------------------------
KMP_API void KMP_CALL kmp_register_log_cb(kmp_log_cb cb, void* user);
KMP_API void KMP_CALL kmp_register_event_cb(kmp_event_cb cb, void* user);

// ---------------------------------------------------------------------------
// Snapshot queries (thread-safe, return counts actually written)
// ---------------------------------------------------------------------------
KMP_API uint32_t KMP_CALL kmp_get_players(kmp_player_info* out, uint32_t max);
KMP_API void     KMP_CALL kmp_get_stats(kmp_stats* out);

// ---------------------------------------------------------------------------
// Admin actions
// ---------------------------------------------------------------------------
KMP_API int32_t KMP_CALL kmp_kick(uint32_t player_id, const char* reason);
KMP_API int32_t KMP_CALL kmp_broadcast_chat(const char* text);
KMP_API int32_t KMP_CALL kmp_inject_posture(uint32_t player_id,
                                            uint8_t posture_flags,
                                            int32_t sticky);
KMP_API void    KMP_CALL kmp_clear_sticky_posture(void);
KMP_API int32_t KMP_CALL kmp_sticky_active(void);
KMP_API uint32_t KMP_CALL kmp_sticky_target(void);
KMP_API uint8_t KMP_CALL kmp_sticky_flags(void);

// ---------------------------------------------------------------------------
// Server-authored entity spawning
// ---------------------------------------------------------------------------
// Returns the assigned id (>= 0x7F000000) or 0 on failure.
KMP_API uint32_t KMP_CALL kmp_spawn_npc(const kmp_npc_spawn_request* req);
KMP_API uint32_t KMP_CALL kmp_spawn_building(const kmp_building_spawn_request* req);
KMP_API int32_t  KMP_CALL kmp_despawn_npc(uint32_t id);
KMP_API int32_t  KMP_CALL kmp_despawn_building(uint32_t id);

// Snapshot queries. out may be NULL; returns the count that would have been
// written (so callers can size the buffer first).
KMP_API uint32_t KMP_CALL kmp_list_spawned_npcs(kmp_npc_spawned* out, uint32_t max);
KMP_API uint32_t KMP_CALL kmp_list_spawned_buildings(kmp_building_spawned* out, uint32_t max);

// Host-published building catalog. `out` may be NULL for a size probe.
typedef struct {
    char stringID[64];
    char name[64];
} kmp_building_catalog_item;
KMP_API uint32_t KMP_CALL kmp_list_building_catalog(kmp_building_catalog_item* out, uint32_t max);

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------
KMP_API int32_t KMP_CALL kmp_save_config(const char* path,
                                         const kmp_server_config* cfg);
KMP_API int32_t KMP_CALL kmp_load_config(const char* path,
                                         kmp_server_config* out);
KMP_API void    KMP_CALL kmp_default_config(kmp_server_config* out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KMP_SERVER_API_H
