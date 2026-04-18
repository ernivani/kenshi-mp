#pragma once

#include <cstdint>
#include <string>

#include "packets.h"

namespace kmp {

inline uint8_t posture_flags_from_anim(uint32_t animation_id) {
    return static_cast<uint8_t>(animation_id & 0xFFu);
}

inline void posture_decode(uint8_t flags,
                           bool& down, bool& ko, bool& rag,
                           bool& dead, bool& chained) {
    down    = (flags & POSTURE_DOWN)        != 0;
    ko      = (flags & POSTURE_UNCONSCIOUS) != 0;
    rag     = (flags & POSTURE_RAGDOLL)     != 0;
    dead    = (flags & POSTURE_DEAD)        != 0;
    chained = (flags & POSTURE_CHAINED)     != 0;
}

inline std::string posture_short_label(uint8_t flags) {
    if (flags == 0) return "CLEAR";
    std::string out;
    auto add = [&](const char* s) {
        if (!out.empty()) out += '|';
        out += s;
    };
    if (flags & POSTURE_DOWN)        add("DOWN");
    if (flags & POSTURE_UNCONSCIOUS) add("KO");
    if (flags & POSTURE_RAGDOLL)     add("RAG");
    if (flags & POSTURE_DEAD)        add("DEAD");
    if (flags & POSTURE_CHAINED)     add("CHAIN");
    return out;
}

} // namespace kmp
