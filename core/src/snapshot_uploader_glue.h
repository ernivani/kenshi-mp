// snapshot_uploader_glue.h — Real Kenshi / ENet / threading bindings for
// the SnapshotUploader. This is the only file in the plugin that wires
// the uploader to its world.
#pragma once

#include <string>

namespace kmp {

struct SnapshotUploadAck;

/// Initialise the singleton uploader with real bindings. Call once from
/// plugin startup.
void snapshot_uploader_glue_init();

/// Shut down: join any background threads, clear state.
void snapshot_uploader_glue_shutdown();

/// Begin upload of the named slot. Called when we confirm the local player
/// is the host (from ui_on_connect_accept).
void snapshot_uploader_glue_start(const std::string& slot);

/// Called every frame from player_sync_tick.
void snapshot_uploader_glue_tick(float dt);

/// Forwards an incoming SNAPSHOT_UPLOAD_ACK from the receive path.
void snapshot_uploader_glue_on_ack(const SnapshotUploadAck& ack);

/// For UI — empty string if idle or no-progress.
std::string snapshot_uploader_glue_progress_text();

/// Current state as int — -1 if not initialised, else SnapshotUploader::State value.
int snapshot_uploader_glue_state_int();

/// Error message if last transition was to FAILED (else empty).
std::string snapshot_uploader_glue_last_error();

} // namespace kmp
