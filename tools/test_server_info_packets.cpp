#include "test_check.h"
#include <cstdio>
#include <cstring>
#include "packets.h"
#include "serialization.h"

using namespace kmp;

static void test_request_roundtrip() {
    ServerInfoRequest orig;
    orig.nonce = 0xDEADBEEF;

    auto buf = pack(orig);
    ServerInfoRequest got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(got.nonce == 0xDEADBEEF);
    KMP_CHECK(got.header.type == PacketType::SERVER_INFO_REQUEST);
    printf("test_request_roundtrip OK\n");
}

static void test_reply_roundtrip() {
    ServerInfoReply orig;
    orig.nonce             = 42;
    orig.player_count      = 3;
    orig.max_players       = 16;
    orig.protocol_version  = PROTOCOL_VERSION;
    orig.password_required = 1;
    std::strncpy(orig.description, "Test server description with punctuation! 1234",
                 sizeof(orig.description) - 1);
    orig.description[sizeof(orig.description) - 1] = '\0';

    auto buf = pack(orig);
    ServerInfoReply got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(got.nonce             == 42);
    KMP_CHECK(got.player_count      == 3);
    KMP_CHECK(got.max_players       == 16);
    KMP_CHECK(got.protocol_version  == PROTOCOL_VERSION);
    KMP_CHECK(got.password_required == 1);
    KMP_CHECK(std::strcmp(got.description,
        "Test server description with punctuation! 1234") == 0);
    printf("test_reply_roundtrip OK\n");
}

int main() {
    test_request_roundtrip();
    test_reply_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
