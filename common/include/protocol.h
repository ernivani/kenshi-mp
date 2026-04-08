#pragma once

#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// KenshiMP protocol constants
// ---------------------------------------------------------------------------

namespace kmp {

static const uint16_t DEFAULT_PORT       = 7777;
static const uint32_t MAX_PLAYERS        = 32;
static const uint32_t TICK_RATE_HZ       = 20;
static const float    TICK_INTERVAL_SEC  = 1.0f / 20;

static const uint8_t  CHANNEL_RELIABLE   = 0;
static const uint8_t  CHANNEL_UNRELIABLE = 1;
static const uint8_t  CHANNEL_COUNT      = 2;

static const float    POSITION_EPSILON   = 0.05f;
static const uint32_t CLIENT_TIMEOUT_MS  = 60000;
static const uint8_t  PROTOCOL_VERSION   = 1;

static const size_t   MAX_NAME_LENGTH    = 32;
static const size_t   MAX_MODEL_LENGTH   = 64;
static const size_t   MAX_CHAT_LENGTH    = 256;

static const float    NPC_SYNC_RADIUS     = 500.0f;
static const float    NPC_SYNC_INTERVAL   = 1.0f / 20;
static const uint16_t MAX_NPC_BATCH       = 49;
static const size_t   MAX_RACE_LENGTH     = 32;
static const size_t   MAX_WEAPON_LENGTH   = 64;
static const size_t   MAX_ARMOUR_LENGTH   = 64;

} // namespace kmp
