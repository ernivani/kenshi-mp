#pragma once

// Returns a stable per-install UUID as a null-terminated string. Generated
// once on first call (via Windows UuidCreate) and persisted to
// %APPDATA%\KenshiMP\client_uuid.txt on disk. Subsequent calls return the
// cached value. Thread-safe after first call.
const char* client_identity_get_uuid();
