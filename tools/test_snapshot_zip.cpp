#include "test_check.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "snapshot_zip.h"
#include "miniz.h"

namespace fs = std::filesystem;

static fs::path make_tempdir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("kmp_zip_test_" + std::string(name)
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

static void test_roundtrip_flat_and_nested() {
    fs::path src = make_tempdir("src");
    write_file(src / "a.txt",                       "hello world");
    write_file(src / "sub" / "b.bin",               std::string("\x00\x01\x02\x03\x04", 5));
    write_file(src / "sub" / "nest" / "c.txt",      "deeply nested");

    std::vector<uint8_t> blob = kmp::zip_directory(src.string());
    KMP_CHECK(!blob.empty());

    fs::path dst = make_tempdir("dst");
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    KMP_CHECK(mz_zip_reader_init_mem(&zip, blob.data(), blob.size(), 0));

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        KMP_CHECK(mz_zip_reader_file_stat(&zip, i, &stat));
        if (stat.m_is_directory) continue;
        fs::path out = dst / stat.m_filename;
        fs::create_directories(out.parent_path());
        KMP_CHECK(mz_zip_reader_extract_to_file(&zip, i, out.string().c_str(), 0));
    }
    mz_zip_reader_end(&zip);

    KMP_CHECK(read_file(dst / "a.txt") == "hello world");
    KMP_CHECK(read_file(dst / "sub" / "b.bin") == std::string("\x00\x01\x02\x03\x04", 5));
    KMP_CHECK(read_file(dst / "sub" / "nest" / "c.txt") == "deeply nested");

    fs::remove_all(src);
    fs::remove_all(dst);
    printf("test_roundtrip_flat_and_nested OK\n");
}

static void test_missing_dir_returns_empty() {
    std::vector<uint8_t> blob = kmp::zip_directory("C:/this/path/does/not/exist/kmp_xyz");
    KMP_CHECK(blob.empty());
    printf("test_missing_dir_returns_empty OK\n");
}

int main() {
    test_roundtrip_flat_and_nested();
    test_missing_dir_returns_empty();
    printf("ALL PASS\n");
    return 0;
}
