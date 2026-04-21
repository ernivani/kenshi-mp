// save_trigger.h — Thin wrapper around Kenshi's SaveFileSystem.
//
// Kenshi's save is asynchronous: saveGame() returns immediately and the
// save happens on Kenshi's own worker thread. Callers poll is_busy()
// until it returns false to know the save is complete.
#pragma once

#include <string>

namespace kmp {

/// Kick off a save of the current world state to the named slot.
/// Returns false if SaveFileSystem is unreachable or the Documents path
/// can't be resolved.
bool save_trigger_start(const std::string& slot_name);

/// True while Kenshi's save worker is still running. Poll after
/// save_trigger_start until this returns false.
bool save_trigger_is_busy();

/// Resolve the on-disk path for a named slot:
///   <Documents>/My Games/Kenshi/save/<slot_name>
/// Returns empty string if Documents resolution fails.
std::string save_trigger_resolve_slot_path(const std::string& slot_name);

} // namespace kmp
