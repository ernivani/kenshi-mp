// load_trigger.h — Thin wrapper around Kenshi's SaveManager::loadGame
// and SaveFileSystem::busy() for the joiner pipeline.
//
// Mirrors save_trigger (A.2) in reverse.
#pragma once

#include <string>

namespace kmp {

/// Kick off a load of the given slot. Returns false if the SaveManager
/// singleton isn't available or Kenshi returns an error. Kenshi's load
/// is asynchronous — poll load_trigger_is_busy() until it returns false.
bool load_trigger_start(const std::string& slot_name);

/// True while Kenshi's load worker is running. Mirrors
/// SaveFileSystem::busy() with a 4-second post-start grace period
/// (same pattern as save_trigger, since loadGame also only queues).
bool load_trigger_is_busy();

/// Same resolver as save_trigger — SaveManager's userSavePath or
/// localSavePath.
std::string load_trigger_resolve_slot_path(const std::string& slot_name);

} // namespace kmp
