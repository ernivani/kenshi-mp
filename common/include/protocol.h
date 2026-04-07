#pragma once

// ---------------------------------------------------------------------------
// KenshiMP protocol constants
// ---------------------------------------------------------------------------

namespace kmp {

constexpr uint16_t DEFAULT_PORT       = 7777;
constexpr uint32_t MAX_PLAYERS        = 32;
constexpr uint32_t TICK_RATE_HZ       = 20;       // position updates per second
constexpr float    TICK_INTERVAL_SEC  = 1.0f / TICK_RATE_HZ;

// ENet channel assignments
constexpr uint8_t  CHANNEL_RELIABLE   = 0;   // connect, disconnect, spawn, chat
constexpr uint8_t  CHANNEL_UNRELIABLE = 1;   // position / state updates
constexpr uint8_t  CHANNEL_COUNT      = 2;

// Movement delta threshold — skip sending if position hasn't changed enough
constexpr float    POSITION_EPSILON   = 0.05f;

// Timeout before server considers a client disconnected (ms)
constexpr uint32_t CLIENT_TIMEOUT_MS  = 10000;

// Protocol version — bump when packet format changes
constexpr uint8_t  PROTOCOL_VERSION   = 1;

// Max length for player display name
constexpr size_t   MAX_NAME_LENGTH    = 32;

// Max length for model/race identifier
constexpr size_t   MAX_MODEL_LENGTH   = 64;

// Max chat message length
constexpr size_t   MAX_CHAT_LENGTH    = 256;

} // namespace kmp
