#include "test_check.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "server_list.h"

namespace fs = std::filesystem;
using namespace kmp;

static fs::path make_tempdir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("kmp_srvlist_" + std::string(name)
        + "_" + std::to_string(std::time(nullptr)));
    fs::create_directories(p);
    return p;
}

static void test_roundtrip() {
    fs::path dir = make_tempdir("roundtrip");
    std::string path = (dir / "servers.json").string();

    std::vector<ServerEntry> orig;
    ServerEntry e1;
    e1.id = "abcd1234"; e1.name = "Bob";
    e1.address = "1.2.3.4"; e1.port = 7777;
    e1.password = "hunter2"; e1.last_joined_ms = 1000;
    orig.push_back(e1);

    ServerEntry e2;
    e2.id = "ffff0000"; e2.name = "Alice's \"LAN\"";
    e2.address = "192.168.1.5"; e2.port = 8888;
    e2.password = ""; e2.last_joined_ms = 0;
    orig.push_back(e2);

    KMP_CHECK(server_list_save_to(path, orig));

    std::vector<ServerEntry> loaded;
    KMP_CHECK(server_list_load_from(path, loaded));
    KMP_CHECK(loaded.size() == 2);
    KMP_CHECK(loaded[0].id == "abcd1234");
    KMP_CHECK(loaded[0].name == "Bob");
    KMP_CHECK(loaded[0].address == "1.2.3.4");
    KMP_CHECK(loaded[0].port == 7777);
    KMP_CHECK(loaded[0].password == "hunter2");
    KMP_CHECK(loaded[0].last_joined_ms == 1000);
    KMP_CHECK(loaded[1].id == "ffff0000");
    KMP_CHECK(loaded[1].name == "Alice's \"LAN\"");

    fs::remove_all(dir);
    printf("test_roundtrip OK\n");
}

static void test_missing_file_returns_false() {
    std::vector<ServerEntry> loaded;
    bool ok = server_list_load_from("C:/does/not/exist/kmp_srv.json", loaded);
    KMP_CHECK(!ok);
    KMP_CHECK(loaded.empty());
    printf("test_missing_file_returns_false OK\n");
}

static void test_corrupt_file_renamed() {
    fs::path dir = make_tempdir("corrupt");
    std::string path = (dir / "servers.json").string();
    std::ofstream f(path);
    f << "not json at all {{{";
    f.close();

    std::vector<ServerEntry> loaded;
    bool ok = server_list_load_from(path, loaded);
    KMP_CHECK(!ok);
    KMP_CHECK(loaded.empty());

    KMP_CHECK(!fs::exists(path));
    bool found_corrupt = false;
    for (auto& p : fs::directory_iterator(dir)) {
        if (p.path().filename().string().find("servers.json.corrupt-") == 0) {
            found_corrupt = true; break;
        }
    }
    KMP_CHECK(found_corrupt);

    fs::remove_all(dir);
    printf("test_corrupt_file_renamed OK\n");
}

static void test_new_id_is_8_hex() {
    std::string id = server_list_new_id();
    KMP_CHECK(id.size() == 8);
    for (char c : id) {
        KMP_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    std::string id2 = server_list_new_id();
    KMP_CHECK(id != id2);
    printf("test_new_id_is_8_hex OK\n");
}

int main() {
    test_roundtrip();
    test_missing_file_returns_false();
    test_corrupt_file_renamed();
    test_new_id_is_8_hex();
    printf("ALL PASS\n");
    return 0;
}
