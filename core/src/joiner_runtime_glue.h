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

} // namespace kmp
