// test_client.cpp — Dummy joiner client for testing NPC sync
//
// Connects to the server as a non-host player, logs all received packets.
// Usage: test_client.exe [host] [port]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>

#include <enet/enet.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

using namespace kmp;

static volatile bool s_running = true;

static void signal_handler(int) { s_running = false; }

static void on_packet(const uint8_t* data, size_t length) {
    PacketHeader header;
    if (!peek_header(data, length, header)) {
        printf("  [!] Invalid packet (too small)\n");
        return;
    }

    switch (header.type) {
    case PacketType::CONNECT_ACCEPT: {
        ConnectAccept pkt;
        if (unpack(data, length, pkt)) {
            printf("  [CONNECT_ACCEPT] player_id=%u\n", pkt.player_id);
        }
        break;
    }
    case PacketType::CONNECT_REJECT: {
        ConnectReject pkt;
        if (unpack(data, length, pkt)) {
            printf("  [CONNECT_REJECT] reason=%s\n", pkt.reason);
        }
        break;
    }
    case PacketType::SPAWN_NPC: {
        SpawnNPC pkt;
        if (unpack(data, length, pkt)) {
            printf("  [SPAWN_NPC] player_id=%u name='%s' model='%s' pos=(%.1f, %.1f, %.1f)\n",
                pkt.player_id, pkt.name, pkt.model, pkt.x, pkt.y, pkt.z);
        }
        break;
    }
    case PacketType::PLAYER_STATE: {
        PlayerState pkt;
        if (unpack(data, length, pkt)) {
            printf("  [PLAYER_STATE] player_id=%u pos=(%.1f, %.1f, %.1f) speed=%.1f\n",
                pkt.player_id, pkt.x, pkt.y, pkt.z, pkt.speed);
        }
        break;
    }
    case PacketType::PLAYER_DISCONNECT: {
        PlayerDisconnect pkt;
        if (unpack(data, length, pkt)) {
            printf("  [PLAYER_DISCONNECT] player_id=%u\n", pkt.player_id);
        }
        break;
    }
    case PacketType::NPC_SPAWN_REMOTE: {
        NPCSpawnRemote pkt;
        if (unpack(data, length, pkt)) {
            printf("  [NPC_SPAWN] npc_id=%u name='%s' race='%s' pos=(%.1f, %.1f, %.1f)\n",
                pkt.npc_id, pkt.name, pkt.race, pkt.x, pkt.y, pkt.z);
        }
        break;
    }
    case PacketType::NPC_BATCH_STATE: {
        NPCBatchHeader batch_hdr;
        if (unpack(data, length, batch_hdr)) {
            printf("  [NPC_BATCH] count=%u", batch_hdr.count);
            size_t offset = sizeof(NPCBatchHeader);
            for (uint16_t i = 0; i < batch_hdr.count && i < 5; ++i) {
                if (offset + sizeof(NPCStateEntry) > length) break;
                NPCStateEntry entry;
                memcpy(&entry, data + offset, sizeof(NPCStateEntry));
                printf(" | npc_%u=(%.0f,%.0f,%.0f)", entry.npc_id, entry.x, entry.y, entry.z);
                offset += sizeof(NPCStateEntry);
            }
            if (batch_hdr.count > 5) printf(" | ... +%u more", batch_hdr.count - 5);
            printf("\n");
        }
        break;
    }
    case PacketType::NPC_DESPAWN_REMOTE: {
        NPCDespawnRemote pkt;
        if (unpack(data, length, pkt)) {
            printf("  [NPC_DESPAWN] npc_id=%u\n", pkt.npc_id);
        }
        break;
    }
    case PacketType::CHAT_MESSAGE: {
        ChatMessage pkt;
        if (unpack(data, length, pkt)) {
            printf("  [CHAT] player_%u: %s\n", pkt.player_id, pkt.message);
        }
        break;
    }
    default:
        printf("  [UNKNOWN] type=0x%02x length=%zu\n", header.type, length);
        break;
    }
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = 7777;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = (uint16_t)atoi(argv[2]);

    printf("=== KenshiMP Test Client ===\n");
    printf("Connecting to %s:%u...\n", host, port);

    if (enet_initialize() != 0) {
        printf("Failed to init ENet\n");
        return 1;
    }

    ENetHost* client = enet_host_create(NULL, 1, CHANNEL_COUNT, 0, 0);
    if (!client) {
        printf("Failed to create ENet host\n");
        enet_deinitialize();
        return 1;
    }

    ENetAddress address;
    enet_address_set_host(&address, host);
    address.port = port;

    ENetPeer* peer = enet_host_connect(client, &address, CHANNEL_COUNT, 0);
    if (!peer) {
        printf("Failed to connect\n");
        enet_host_destroy(client);
        enet_deinitialize();
        return 1;
    }

    // Wait for connection
    ENetEvent event;
    if (enet_host_service(client, &event, 5000) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT) {
        printf("Connected! Sending join request...\n");
    } else {
        printf("Connection failed (timeout)\n");
        enet_peer_reset(peer);
        enet_host_destroy(client);
        enet_deinitialize();
        return 1;
    }

    // Send connect request (NOT host)
    ConnectRequest req;
    strncpy(req.name, "TestBot", MAX_NAME_LENGTH - 1);
    strncpy(req.model, "greenlander", MAX_MODEL_LENGTH - 1);
    req.is_host = 0;  // joiner, not host

    std::vector<uint8_t> buf = pack(req);
    ENetPacket* pkt = enet_packet_create(buf.data(), buf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, CHANNEL_RELIABLE, pkt);
    enet_host_flush(client);

    printf("Joined as non-host. Listening for packets... (Ctrl+C to quit)\n\n");

    signal(SIGINT, signal_handler);

    // Main loop — poll and print
    while (s_running) {
        while (enet_host_service(client, &event, 100) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_RECEIVE:
                on_packet(event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                printf("\n[!] Disconnected from server\n");
                s_running = false;
                break;
            default:
                break;
            }
        }
    }

    enet_peer_disconnect_now(peer, 0);
    enet_host_destroy(client);
    enet_deinitialize();
    printf("Test client stopped.\n");
    return 0;
}
