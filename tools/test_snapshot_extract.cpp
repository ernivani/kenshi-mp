#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "snapshot_extract.h"
#include "snapshot_zip.h"

namespace fs = std::filesystem;
using namespace kmp;

static fs::path make_tempdir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("kmp_extract_" + std::string(name)
        + "_" + std::to_string(std::time(nullptr)));
    fs::create_directories(p);
    return p;
}

static void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(contents.data(), contents.size());
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

static void test_roundtrip() {
    fs::path src = make_tempdir("src");
    write_file(src / "a.txt", "hello");
    write_file(src / "sub" / "b.bin", std::string("\x00\x01\x02", 3));
    write_file(src / "deep" / "nest" / "c.txt", "deep");

    std::vector<uint8_t> blob = kmp::zip_directory(src.string());
    KMP_CHECK(!blob.empty());

    fs::path zip_path = make_tempdir("zip") / "out.zip";
    {
        std::ofstream f(zip_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(blob.data()), blob.size());
    }

    fs::path dst = make_tempdir("dst");
    bool ok = kmp::extract_zip_to_dir(zip_path.string(), dst.string());
    KMP_CHECK(ok);

    KMP_CHECK(read_file(dst / "a.txt") == "hello");
    KMP_CHECK(read_file(dst / "sub" / "b.bin") == std::string("\x00\x01\x02", 3));
    KMP_CHECK(read_file(dst / "deep" / "nest" / "c.txt") == "deep");

    fs::remove_all(src);
    fs::remove_all(zip_path.parent_path());
    fs::remove_all(dst);
    printf("test_roundtrip OK\n");
}

static void test_missing_zip_returns_false() {
    fs::path dst = make_tempdir("dst-missing");
    bool ok = kmp::extract_zip_to_dir("C:/does/not/exist/kmp.zip", dst.string());
    KMP_CHECK(!ok);
    fs::remove_all(dst);
    printf("test_missing_zip_returns_false OK\n");
}

int main() {
    test_roundtrip();
    test_missing_zip_returns_false();
    printf("ALL PASS\n");
    return 0;
}
