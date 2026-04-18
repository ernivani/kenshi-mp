#pragma once

#include <cstdint>
#include <string>

#include "server_api.h"

namespace kmp {

// Fan-out hooks invoked from session/admin code.
void events_emit_player_connected(uint32_t id, const std::string& name);
void events_emit_player_disconnected(uint32_t id, const std::string& name);
void events_emit_chat(uint32_t id, const std::string& author, const std::string& text);
void events_emit_posture(uint32_t id, const std::string& name,
                        uint8_t old_flags, uint8_t new_flags);

// Registration (called from core.cpp).
void events_set_callback(kmp_event_cb cb, void* user);

} // namespace kmp
