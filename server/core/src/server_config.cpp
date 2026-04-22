#include "server_config.h"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace kmp {

using json = nlohmann::json;

ServerConfig load_config(const std::string& path) {
    ServerConfig cfg;
    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::info("No config file found at '{}', using defaults", path);
        return cfg;
    }
    try {
        json j = json::parse(file);
        if (j.contains("port"))          cfg.port          = j["port"];
        if (j.contains("max_players"))   cfg.max_players   = j["max_players"];
        if (j.contains("server_name"))   cfg.server_name   = j["server_name"];
        if (j.contains("view_distance")) cfg.view_distance = j["view_distance"];
        if (j.contains("description")) cfg.description = j["description"].get<std::string>();
        if (j.contains("password"))    cfg.password    = j["password"].get<std::string>();
    } catch (const json::exception& e) {
        spdlog::warn("Failed to parse config: {}", e.what());
    }
    return cfg;
}

bool save_config(const std::string& path, const ServerConfig& cfg) {
    try {
        json j;
        j["port"]          = cfg.port;
        j["max_players"]   = cfg.max_players;
        j["server_name"]   = cfg.server_name;
        j["view_distance"] = cfg.view_distance;
        j["description"] = cfg.description;
        j["password"]    = cfg.password;
        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open '{}' for writing", path);
            return false;
        }
        file << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("save_config failed: {}", e.what());
        return false;
    }
}

} // namespace kmp
