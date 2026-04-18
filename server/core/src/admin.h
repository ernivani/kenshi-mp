#pragma once

#include <cstdint>
#include <string>

namespace kmp {

void admin_kick(uint32_t player_id, const char* reason);
void admin_broadcast_chat(const std::string& text);
void admin_request_shutdown();

// Posture injection. Builds a PlayerState with target's last known pos and
// the given posture flags OR'd into the low 8 bits of animation_id, then
// broadcasts (target included) so every client exercises the same receive path.
void admin_inject_posture(uint32_t target_player_id, uint8_t posture_flags, bool sticky);
void admin_clear_sticky_posture();
bool admin_sticky_active();
uint32_t admin_sticky_target();
uint8_t admin_sticky_flags();

// Called once per frame from main. Resends the sticky posture packet on a
// ~500 ms cadence so it survives the client's next real PlayerState upload.
void admin_tick();

// Set by main.cpp so admin_request_shutdown can flip the loop flag.
void admin_set_running_flag(volatile bool* running);

} // namespace kmp
