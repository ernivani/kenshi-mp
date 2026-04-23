// character_store.h — Per-server joiner character persistence.
//
// Stores the serialized PlayerInterface blob (raw bytes of a Kenshi
// GameData file) keyed by client_uuid. Allows a joiner to restore their
// exact squad on reconnect instead of spawning a fresh Wanderer.
//
// On-disk layout: server_data/characters/<uuid>.dat (raw blob bytes).
// In-RAM cache: populated on demand; writes go straight to disk.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kmp {

// Initialise storage directory. Creates server_data/characters/ if needed.
void character_store_init();

// Returns true if a blob is available for `uuid`. On success, copies
// the bytes into `out`. UUID is matched case-sensitively and must be
// non-empty; empty UUIDs are never matched.
bool character_store_get(const std::string& uuid, std::vector<uint8_t>& out);

// Persist `blob` for `uuid`. Overwrites any existing entry. Silently
// drops empty UUIDs (no per-machine identity → nothing to persist).
void character_store_set(const std::string& uuid,
                         const uint8_t* blob, size_t len);

} // namespace kmp
