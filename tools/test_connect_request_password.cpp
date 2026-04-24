#include "test_check.h"
#include <cstdio>
#include <cstring>
#include "packets.h"
#include "serialization.h"

using namespace kmp;

static void test_password_roundtrip() {
    ConnectRequest orig;
    std::strncpy(orig.name, "Bob",          MAX_NAME_LENGTH - 1);
    std::strncpy(orig.model, "greenlander",  MAX_MODEL_LENGTH - 1);
    orig.is_host = 0;
    std::strncpy(orig.client_uuid, "some-uuid", sizeof(orig.client_uuid) - 1);
    std::strncpy(orig.password, "hunter2",  MAX_PASSWORD_LENGTH - 1);

    auto buf = pack(orig);
    ConnectRequest got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(std::strcmp(got.name,     "Bob") == 0);
    KMP_CHECK(std::strcmp(got.model,    "greenlander") == 0);
    KMP_CHECK(got.is_host == 0);
    KMP_CHECK(std::strcmp(got.password, "hunter2") == 0);
    printf("test_password_roundtrip OK\n");
}

static void test_empty_password_roundtrip() {
    ConnectRequest orig;
    std::strncpy(orig.name, "Alice", MAX_NAME_LENGTH - 1);
    // password left zero (default-constructed struct is memset to 0).

    auto buf = pack(orig);
    ConnectRequest got;
    KMP_CHECK(unpack(buf.data(), buf.size(), got));
    KMP_CHECK(got.password[0] == '\0');
    printf("test_empty_password_roundtrip OK\n");
}

int main() {
    test_password_roundtrip();
    test_empty_password_roundtrip();
    printf("ALL PASS\n");
    return 0;
}
