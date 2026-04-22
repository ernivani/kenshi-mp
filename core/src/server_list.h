// server_list.h — Persistent list of multiplayer server entries.
//
// Stored as flat JSON at <Documents>/My Games/Kenshi/KenshiMP/servers.json.
// Hand-rolled parser — v100 toolchain can't link nlohmann/json.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kmp {

struct ServerEntry {
    std::string id;             // 8-hex-char, generated via server_list_new_id()
    std::string name;
    std::string address;
    uint16_t    port;
    std::string password;
    uint64_t    last_joined_ms;

    ServerEntry() : port(0), last_joined_ms(0) {}
};

bool server_list_load_from(const std::string& path, std::vector<ServerEntry>& out);
bool server_list_save_to(const std::string& path, const std::vector<ServerEntry>& in);
std::string server_list_default_path();
bool server_list_load(std::vector<ServerEntry>& out);
bool server_list_save(const std::vector<ServerEntry>& in);
std::string server_list_new_id();

} // namespace kmp
