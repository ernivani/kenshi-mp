// relay.cpp — Broadcast/relay packets to connected clients
//
// Provides helpers used by session.cpp to send packets to individual
// clients or broadcast to all.

#include <cstdint>
#include <atomic>
#include <enet/enet.h>
#include "protocol.h"

namespace kmp {

static ENetHost* s_relay_host = nullptr;

static std::atomic<uint64_t> s_packets_out{0};
static std::atomic<uint64_t> s_bytes_out{0};
static std::atomic<uint64_t> s_packets_in{0};
static std::atomic<uint64_t> s_bytes_in{0};

void relay_init(ENetHost* host) {
    s_relay_host = host;
}

// Send a packet to a specific peer
void relay_send_to(ENetPeer* peer, const uint8_t* data, size_t length, bool reliable) {
    if (!peer) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    uint8_t channel = reliable ? CHANNEL_RELIABLE : CHANNEL_UNRELIABLE;

    ENetPacket* packet = enet_packet_create(data, length, flags);
    enet_peer_send(peer, channel, packet);

    s_packets_out.fetch_add(1, std::memory_order_relaxed);
    s_bytes_out.fetch_add(length, std::memory_order_relaxed);
}

// Broadcast a packet to all connected peers, optionally excluding one
void relay_broadcast(ENetPeer* exclude, const uint8_t* data, size_t length, bool reliable) {
    if (!s_relay_host) return;

    uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    uint8_t channel = reliable ? CHANNEL_RELIABLE : CHANNEL_UNRELIABLE;

    for (size_t i = 0; i < s_relay_host->peerCount; ++i) {
        ENetPeer* peer = &s_relay_host->peers[i];
        if (peer->state != ENET_PEER_STATE_CONNECTED) continue;
        if (peer == exclude) continue;

        ENetPacket* packet = enet_packet_create(data, length, flags);
        enet_peer_send(peer, channel, packet);

        s_packets_out.fetch_add(1, std::memory_order_relaxed);
        s_bytes_out.fetch_add(length, std::memory_order_relaxed);
    }
}

void relay_record_incoming(size_t length) {
    s_packets_in.fetch_add(1, std::memory_order_relaxed);
    s_bytes_in.fetch_add(length, std::memory_order_relaxed);
}

uint64_t relay_stat_packets_out() { return s_packets_out.load(std::memory_order_relaxed); }
uint64_t relay_stat_bytes_out()   { return s_bytes_out.load(std::memory_order_relaxed); }
uint64_t relay_stat_packets_in()  { return s_packets_in.load(std::memory_order_relaxed); }
uint64_t relay_stat_bytes_in()    { return s_bytes_in.load(std::memory_order_relaxed); }

} // namespace kmp
