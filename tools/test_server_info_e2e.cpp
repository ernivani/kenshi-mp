// test_server_info_e2e.cpp — Live ping: connect to a headless server,
// send SERVER_INFO_REQUEST, verify a valid REPLY comes back.
//
// Requires: ./build_server/bin/Release/kenshi-mp-server-headless.exe
// running on 127.0.0.1:7777.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <enet/enet.h>

#include "packets.h"
#include "serialization.h"

using namespace kmp;

static int fail(const char* msg) { fprintf(stderr, "FAIL: %s\n", msg); return 1; }

int main(int argc, char** argv) {
    const char* host_ip   = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t    enet_port = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 7777;

    if (enet_initialize() != 0) return fail("enet_initialize");

    ENetHost* client = enet_host_create(NULL, 1, 2, 0, 0);
    if (!client) return fail("enet_host_create");

    ENetAddress addr;
    if (enet_address_set_host(&addr, host_ip) != 0) return fail("enet_address_set_host");
    addr.port = enet_port;

    ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
    if (!peer) return fail("enet_host_connect");

    ENetEvent ev;
    bool connected = false;
    for (int i = 0; i < 50 && !connected; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
    }
    if (!connected) return fail("ENet handshake timeout");

    ServerInfoRequest req; req.nonce = 0x12345678;
    auto buf = pack(req);
    ENetPacket* pkt = enet_packet_create(buf.data(), buf.size(),
                                         ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, pkt);
    enet_host_flush(client);

    bool got_reply = false;
    for (int i = 0; i < 50 && !got_reply; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_RECEIVE) {
            PacketHeader h;
            if (peek_header(ev.packet->data, ev.packet->dataLength, h) &&
                h.type == PacketType::SERVER_INFO_REPLY) {
                ServerInfoReply reply;
                if (unpack(ev.packet->data, ev.packet->dataLength, reply)) {
                    if (reply.nonce != 0x12345678) {
                        fprintf(stderr, "nonce mismatch: got 0x%x\n", reply.nonce);
                        enet_packet_destroy(ev.packet);
                        return 1;
                    }
                    printf("SERVER_INFO_REPLY: players=%u/%u version=%u "
                           "password_required=%u desc=\"%s\"\n",
                           reply.player_count, reply.max_players,
                           reply.protocol_version, reply.password_required,
                           reply.description);
                    got_reply = true;
                }
            }
            enet_packet_destroy(ev.packet);
        }
    }
    if (!got_reply) return fail("no SERVER_INFO_REPLY received");

    enet_peer_disconnect(peer, 0);
    enet_host_flush(client);
    enet_host_destroy(client);
    enet_deinitialize();
    printf("ALL PASS\n");
    return 0;
}
