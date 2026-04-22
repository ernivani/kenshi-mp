#include "snapshot_extract.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "miniz.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kmp {

namespace {

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

static bool create_dirs_w(const std::wstring& path) {
    if (path.empty()) return true;
    size_t start = 0;
    while (true) {
        size_t slash = path.find_first_of(L"\\/", start);
        std::wstring prefix = (slash == std::wstring::npos)
            ? path : path.substr(0, slash);
        if (!prefix.empty() && prefix.back() != L':') {
            if (!CreateDirectoryW(prefix.c_str(), NULL)) {
                DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) return false;
            }
        }
        if (slash == std::wstring::npos) return true;
        start = slash + 1;
    }
}

} // namespace

bool extract_zip_to_dir(const std::string& zip_path,
                        const std::string& dst_dir) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) {
        return false;
    }

    std::wstring wdst = utf8_to_wide(dst_dir);
    if (!create_dirs_w(wdst)) {
        mz_zip_reader_end(&zip);
        return false;
    }

    mz_uint count = mz_zip_reader_get_num_files(&zip);
    bool ok = true;
    for (mz_uint i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) { ok = false; break; }
        if (stat.m_is_directory) continue;

        std::string entry_name = stat.m_filename;
        for (size_t k = 0; k < entry_name.size(); ++k) {
            if (entry_name[k] == '/') entry_name[k] = '\\';
        }

        std::string out_path = dst_dir + "\\" + entry_name;
        std::wstring wout = utf8_to_wide(out_path);

        size_t last_sep = wout.find_last_of(L'\\');
        if (last_sep != std::wstring::npos) {
            std::wstring parent = wout.substr(0, last_sep);
            if (!create_dirs_w(parent)) { ok = false; break; }
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, out_path.c_str(), 0)) {
            ok = false;
            break;
        }
    }

    mz_zip_reader_end(&zip);
    return ok;
}

} // namespace kmp
