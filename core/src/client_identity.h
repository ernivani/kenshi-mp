#pragma once

#include <string>

// Returns a stable per-install UUID as a null-terminated string. Generated
// once on first call (via Windows UuidCreate) and persisted to
// %APPDATA%\KenshiMP\client_uuid.txt on disk. Subsequent calls return the
// cached value. Thread-safe after first call.
const char* client_identity_get_uuid();

// Persisted character identity (name + Kenshi model/race). Read from
// %APPDATA%\KenshiMP\character.txt (2 lines: name, model). Returns
// sensible defaults if the file doesn't exist yet.
const std::string& client_identity_get_name();
const std::string& client_identity_get_model();

// Persist new values. Empty strings keep defaults.
void client_identity_set_name(const std::string& name);
void client_identity_set_model(const std::string& model);

// True once the user has explicitly saved their identity via
// set_name/set_model. False until the character-creation modal is
// confirmed, so the join flow knows whether to prompt.
bool client_identity_has_custom();
