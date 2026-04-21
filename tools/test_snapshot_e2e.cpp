// test_snapshot_e2e.cpp — Upload a blob via ENet SNAPSHOT_UPLOAD_*, then
// download it via HTTP GET /snapshot, verify bytes match.
//
// Requires: server running on 127.0.0.1:7777 (ENet) + 7778 (HTTP).
// Usage: test-snapshot-e2e [host] [port]

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

#include <enet/enet.h>

#include "packets.h"
#include "serialization.h"
#include "picosha2.h"

#include "httplib.h"

using namespace kmp;

static int fail(const char* msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(int argc, char** argv) {
    const char* host_ip    = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t    enet_port  = argc > 2 ? static_cast<uint16_t>(atoi(argv[2])) : 7777;
    uint16_t    http_port  = static_cast<uint16_t>(enet_port + 1);

    if (enet_initialize() != 0) return fail("enet_initialize");

    ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!client) return fail("enet_host_create");

    ENetAddress addr;
    enet_address_set_host(&addr, host_ip);
    addr.port = enet_port;

    ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
    if (!peer) return fail("enet_host_connect");

    ENetEvent ev;
    bool connected = false;
    for (int i = 0; i < 50 && !connected; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_CONNECT) {
            connected = true;
        }
    }
    if (!connected) return fail("ENet handshake timeout (server running?)");
    printf("Connected to %s:%u\n", host_ip, enet_port);

    // Build a 1 MB blob.
    std::vector<uint8_t> blob(1024 * 1024);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i & 0xFF);

    uint8_t sha[32];
    picosha2::hash256(blob.begin(), blob.end(), sha, sha + 32);

    // BEGIN
    SnapshotUploadBegin begin;
    begin.upload_id  = 1;
    begin.rev        = 1;
    begin.total_size = blob.size();
    std::memcpy(begin.sha256, sha, 32);
    auto bbuf = pack(begin);
    ENetPacket* bpkt = enet_packet_create(bbuf.data(), bbuf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, bpkt);

    // CHUNK (16 KB each)
    const uint16_t chunk_sz = 16 * 1024;
    for (size_t off = 0; off < blob.size(); off += chunk_sz) {
        uint16_t len = static_cast<uint16_t>(std::min<size_t>(chunk_sz, blob.size() - off));
        SnapshotUploadChunk ch;
        ch.upload_id = 1;
        ch.offset    = static_cast<uint32_t>(off);
        ch.length    = len;
        auto cbuf = pack_with_tail(ch, blob.data() + off, len);
        ENetPacket* cpkt = enet_packet_create(cbuf.data(), cbuf.size(), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, 0, cpkt);
    }

    // END
    SnapshotUploadEnd end;
    end.upload_id = 1;
    auto ebuf = pack(end);
    ENetPacket* epkt = enet_packet_create(ebuf.data(), ebuf.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, epkt);
    enet_host_flush(client);

    // Wait for ACK
    bool acked = false;
    for (int i = 0; i < 100 && !acked; ++i) {
        if (enet_host_service(client, &ev, 100) > 0 &&
            ev.type == ENET_EVENT_TYPE_RECEIVE) {
            PacketHeader h;
            if (peek_header(ev.packet->data, ev.packet->dataLength, h) &&
                h.type == PacketType::SNAPSHOT_UPLOAD_ACK) {
                SnapshotUploadAck ack;
                if (unpack(ev.packet->data, ev.packet->dataLength, ack)) {
                    if (ack.accepted != 1 || ack.error_code != 0) {
                        fprintf(stderr, "ACK rejected: accepted=%u err=%u\n",
                                ack.accepted, ack.error_code);
                        enet_packet_destroy(ev.packet);
                        enet_host_destroy(client);
                        enet_deinitialize();
                        return 1;
                    }
                    acked = true;
                }
            }
            enet_packet_destroy(ev.packet);
        }
    }
    if (!acked) return fail("no ACK received");
    printf("Upload ACKed\n");

    enet_peer_disconnect(peer, 0);
    enet_host_flush(client);
    enet_host_destroy(client);
    enet_deinitialize();

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client cli(host_ip, http_port);
    auto res = cli.Get("/snapshot");
    if (!res) return fail("HTTP GET /snapshot failed (sidecar running?)");
    if (res->status != 200) { fprintf(stderr, "HTTP status=%d\n", res->status); return 1; }
    if (res->body.size() != blob.size()) {
        fprintf(stderr, "size mismatch: got %zu expected %zu\n", res->body.size(), blob.size());
        return 1;
    }
    if (std::memcmp(res->body.data(), blob.data(), blob.size()) != 0) {
        return fail("downloaded bytes do not match uploaded blob");
    }
    printf("HTTP download matches uploaded blob (%zu bytes)\n", blob.size());
    printf("ALL PASS\n");
    return 0;
}
