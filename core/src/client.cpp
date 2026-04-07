// client.cpp — ENet client for connecting to KenshiMP server
//
// Manages the network connection: connect, send, receive, disconnect.
// Runs on the game thread — poll() is called each frame from player_sync.

#include <enet/enet.h>
#include <cstring>
#include <string>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ENetHost*   s_client  = NULL;
static ENetPeer*   s_peer    = NULL;
static bool        s_connected = false;
static uint32_t    s_local_id  = 0;

// Callback for received packets — set by player_sync
typedef void (*PacketCallback)(const uint8_t* data, size_t length);
static PacketCallback s_on_packet;

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void client_init() {
    if (enet_initialize() != 0) {
        // TODO: log error
        return;
    }

    s_client = enet_host_create(
        NULL,            // client, no binding
        1,                  // one outgoing connection
        CHANNEL_COUNT,
        0,                  // unlimited incoming bandwidth
        0                   // unlimited outgoing bandwidth
    );
}

void client_shutdown() {
    if (s_peer) {
        enet_peer_disconnect_now(s_peer, 0);
        s_peer = NULL;
    }
    if (s_client) {
        enet_host_destroy(s_client);
        s_client = NULL;
    }
    enet_deinitialize();
    s_connected = false;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
bool client_connect(const char* host, uint16_t port) {
    if (!s_client || s_peer) return false;

    ENetAddress address;
    enet_address_set_host(&address, host);
    address.port = port;

    s_peer = enet_host_connect(s_client, &address, CHANNEL_COUNT, 0);
    if (!s_peer) return false;

    // Wait up to 5 seconds for connection
    ENetEvent event;
    if (enet_host_service(s_client, &event, 5000) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT) {
        s_connected = true;
        return true;
    }

    enet_peer_reset(s_peer);
    s_peer = NULL;
    return false;
}

void client_disconnect() {
    if (!s_peer) return;

    enet_peer_disconnect(s_peer, 0);

    // Allow up to 3 seconds for graceful disconnect
    ENetEvent event;
    while (enet_host_service(s_client, &event, 3000) > 0) {
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            break;
        }
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
    }

    enet_peer_reset(s_peer);
    s_peer = NULL;
    s_connected = false;
    s_local_id = 0;
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------
void client_send_reliable(const uint8_t* data, size_t length) {
    if (!s_peer || !s_connected) return;

    ENetPacket* packet = enet_packet_create(
        data, length, ENET_PACKET_FLAG_RELIABLE
    );
    enet_peer_send(s_peer, CHANNEL_RELIABLE, packet);
    enet_host_flush(s_client);
}

void client_send_unreliable(const uint8_t* data, size_t length) {
    if (!s_peer || !s_connected) return;

    ENetPacket* packet = enet_packet_create(
        data, length, ENET_PACKET_FLAG_UNSEQUENCED
    );
    enet_peer_send(s_peer, CHANNEL_UNRELIABLE, packet);
    enet_host_flush(s_client);
}

template <typename T>
void client_send(const T& pkt, bool reliable) {
    auto buf = pack(pkt);
    if (reliable) {
        client_send_reliable(buf.data(), buf.size());
    } else {
        client_send_unreliable(buf.data(), buf.size());
    }
}

// ---------------------------------------------------------------------------
// Poll — call once per frame
// ---------------------------------------------------------------------------
void client_set_packet_callback(PacketCallback cb) {
    s_on_packet = cb;
}

void client_poll() {
    if (!s_client) return;

    ENetEvent event;
    while (enet_host_service(s_client, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE:
            if (s_on_packet) {
                s_on_packet(event.packet->data, event.packet->dataLength);
            }
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            s_connected = false;
            s_peer = NULL;
            s_local_id = 0;
            break;

        default:
            break;
        }
    }

    enet_host_flush(s_client);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
bool     client_is_connected()   { return s_connected; }
uint32_t client_get_local_id()   { return s_local_id; }
void     client_set_local_id(uint32_t id) { s_local_id = id; }

} // namespace kmp
