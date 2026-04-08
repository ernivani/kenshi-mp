// main.cpp — KenshiMP dedicated server entry point
//
// Standalone console app. Listens for ENet connections, manages sessions,
// relays player state between clients.

#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <string>
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>

#include <enet/enet.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "protocol.h"
#include "packets.h"
#include "serialization.h"

namespace kmp {
    // session.cpp
    void session_init();
    void session_on_connect(ENetPeer* peer);
    void session_on_disconnect(ENetPeer* peer);
    void session_on_packet(ENetPeer* peer, const uint8_t* data, size_t length);
    void session_check_timeouts();

    // world_state.cpp
    void world_state_init();

    // relay.cpp
    void relay_init(ENetHost* host);
}

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct ServerConfig {
    uint16_t    port         = kmp::DEFAULT_PORT;
    uint32_t    max_players  = kmp::MAX_PLAYERS;
    std::string server_name  = "KenshiMP Server";
    float       view_distance = 5000.0f;  // relay filtering radius
};

static ServerConfig load_config(const std::string& path) {
    ServerConfig cfg;
    std::ifstream file(path);
    if (file.is_open()) {
        try {
            json j = json::parse(file);
            if (j.contains("port"))          cfg.port          = j["port"];
            if (j.contains("max_players"))   cfg.max_players   = j["max_players"];
            if (j.contains("server_name"))   cfg.server_name   = j["server_name"];
            if (j.contains("view_distance")) cfg.view_distance = j["view_distance"];
        } catch (const json::exception& e) {
            spdlog::warn("Failed to parse config: {}", e.what());
        }
    } else {
        spdlog::info("No config file found at '{}', using defaults", path);
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static volatile bool s_running = true;

static void signal_handler(int) {
    s_running = false;
}

int main(int argc, char* argv[]) {
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("KenshiMP Dedicated Server v{}.{}.{}",
        0, 1, 0);

    // Load config
    std::string config_path = (argc > 1) ? argv[1] : "server_config.json";
    ServerConfig cfg = load_config(config_path);

    spdlog::info("Port: {}, Max players: {}, Name: {}",
        cfg.port, cfg.max_players, cfg.server_name);

    // Init ENet
    if (enet_initialize() != 0) {
        spdlog::error("Failed to initialize ENet");
        return 1;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = cfg.port;

    ENetHost* server = enet_host_create(
        &address,
        cfg.max_players,
        kmp::CHANNEL_COUNT,
        0, 0
    );

    if (!server) {
        spdlog::error("Failed to create ENet host on port {}", cfg.port);
        enet_deinitialize();
        return 1;
    }

    // Init subsystems
    kmp::world_state_init();
    kmp::session_init();
    kmp::relay_init(server);

    spdlog::info("Server listening on port {}", cfg.port);

    // Signal handling for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ---------------------------------------------------------------------------
    // Main loop
    // ---------------------------------------------------------------------------
    auto last_timeout_check = std::chrono::steady_clock::now();

    while (s_running) {
        ENetEvent event;
        // Poll with 10ms timeout to keep the loop responsive
        while (enet_host_service(server, &event, 10) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                spdlog::info("Client connected from {}:{}",
                    event.peer->address.host, event.peer->address.port);
                kmp::session_on_connect(event.peer);
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                kmp::session_on_packet(
                    event.peer,
                    event.packet->data,
                    event.packet->dataLength
                );
                enet_packet_destroy(event.packet);
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                spdlog::info("Client disconnected");
                kmp::session_on_disconnect(event.peer);
                break;

            default:
                break;
            }
        }

        // Periodic timeout check
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_timeout_check).count() >= 5) {
            kmp::session_check_timeouts();
            last_timeout_check = now;
        }
    }

    // Shutdown
    spdlog::info("Shutting down...");
    enet_host_destroy(server);
    enet_deinitialize();
    spdlog::info("Server stopped");

    return 0;
}
