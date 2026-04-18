#pragma once

#include <cstdint>
#include <string>

#include "protocol.h"

namespace kmp {

struct ServerConfig {
    uint16_t    port          = DEFAULT_PORT;
    uint32_t    max_players   = MAX_PLAYERS;
    std::string server_name   = "KenshiMP Server";
    float       view_distance = 5000.0f;
};

ServerConfig load_config(const std::string& path);
bool         save_config(const std::string& path, const ServerConfig& cfg);

} // namespace kmp
