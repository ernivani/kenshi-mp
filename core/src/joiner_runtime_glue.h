// joiner_runtime_glue.h — Real Win32 / WinHTTP / miniz / ENet / Kenshi
// bindings for JoinerRuntime. Only file in the plugin that spans all
// those worlds.
#pragma once

#include <cstdint>
#include <string>

namespace kmp {

struct ServerEntry;

void joiner_runtime_glue_init();
void joiner_runtime_glue_shutdown();

void joiner_runtime_glue_start(const ServerEntry& entry);
void joiner_runtime_glue_cancel();
void joiner_runtime_glue_tick(float dt);
void joiner_runtime_glue_on_connect_accept(uint32_t player_id);
void joiner_runtime_glue_on_connect_reject(const std::string& reason);

int         joiner_runtime_glue_state_int();
std::string joiner_runtime_glue_stage_label();
std::string joiner_runtime_glue_progress_text();
std::string joiner_runtime_glue_last_error();

// True if we arrived in-game via the A.4 snapshot pipeline — means the
// local world was freshly loaded from the host's save and must NOT be
// wiped on CONNECT_ACCEPT. Stays true for the rest of the session.
bool        joiner_runtime_glue_did_snapshot_join();

// True while the background async-connect thread is running enet_host_service
// for the handshake — the main thread must NOT touch the ENet host during
// this window (not thread-safe). player_sync_tick checks this to gate its
// client_poll().
bool        joiner_runtime_glue_enet_connect_busy();

// Tell the background async-connect keepalive loop to exit. Call from
// the main thread once it's ready to take over client_poll duties —
// otherwise the peer times out during SaveManager::load while nothing
// is servicing ENet. Idempotent.
void        joiner_runtime_glue_stop_keepalive();

} // namespace kmp
