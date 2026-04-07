// relay.cpp — Broadcast/relay packets to connected clients
//
// Provides helpers used by session.cpp to send packets to individual
// clients or broadcast to all.

#include <enet/enet.h>
#include "protocol.h"

namespace kmp {

static ENetHost* s_host = nullptr;

void relay_init(ENetHost* host) {
    s_host = host;
}

// Send a packet to a specific peer
void relay_send_to(ENetPeer* peer, const uint8_t* data, size_t length, bool reliable) {
    if (!peer) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    uint8_t channel = reliable ? CHANNEL_RELIABLE : CHANNEL_UNRELIABLE;

    ENetPacket* packet = enet_packet_create(data, length, flags);
    enet_peer_send(peer, channel, packet);
}

// Broadcast a packet to all connected peers, optionally excluding one
void relay_broadcast(ENetPeer* exclude, const uint8_t* data, size_t length, bool reliable) {
    if (!s_host) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    uint8_t channel = reliable ? CHANNEL_RELIABLE : CHANNEL_UNRELIABLE;

    for (size_t i = 0; i < s_host->peerCount; ++i) {
        ENetPeer* peer = &s_host->peers[i];
        if (peer->state != ENET_PEER_STATE_CONNECTED) continue;
        if (peer == exclude) continue;

        ENetPacket* packet = enet_packet_create(data, length, flags);
        enet_peer_send(peer, channel, packet);
    }
}

} // namespace kmp
