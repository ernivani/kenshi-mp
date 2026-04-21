#include "snapshot_zip.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "miniz.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace kmp {

namespace {

static std::string make_archive_name(const std::wstring& full_w,
                                     const std::wstring& root_w) {
    std::wstring rel_w;
    if (full_w.size() > root_w.size() &&
        full_w.compare(0, root_w.size(), root_w) == 0) {
        rel_w = full_w.substr(root_w.size());
    } else {
        rel_w = full_w;
    }
    while (!rel_w.empty() && (rel_w[0] == L'\\' || rel_w[0] == L'/')) {
        rel_w.erase(0, 1);
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, rel_w.c_str(),
                                     static_cast<int>(rel_w.size()),
                                     nullptr, 0, nullptr, nullptr);
    std::string rel(needed, '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, rel_w.c_str(),
                            static_cast<int>(rel_w.size()),
                            &rel[0], needed, nullptr, nullptr);
    }
    for (std::string::size_type i = 0; i < rel.size(); ++i)
        if (rel[i] == '\\') rel[i] = '/';
    return rel;
}

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                     static_cast<int>(s.size()),
                                     nullptr, 0);
    std::wstring w(needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], needed);
    return w;
}

static bool walk(const std::wstring& root_w,
                 const std::wstring& cur_w,
                 mz_zip_archive& zip) {
    std::wstring pattern = cur_w + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        std::wstring child = cur_w + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!walk(root_w, child, zip)) { ok = false; break; }
            continue;
        }

        std::ifstream f;
        f.open(child.c_str(), std::ios::binary);
        if (!f.is_open()) { ok = false; break; }
        std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());

        std::string archive_name = make_archive_name(child, root_w);
        if (!mz_zip_writer_add_mem(&zip, archive_name.c_str(),
                                   buf.data(), buf.size(),
                                   MZ_DEFAULT_LEVEL)) {
            ok = false;
            break;
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return ok;
}

} // namespace

std::vector<uint8_t> zip_directory(const std::string& abs_path) {
    std::wstring root_w = utf8_to_wide(abs_path);
    while (!root_w.empty() &&
           (root_w[root_w.size() - 1] == L'\\' || root_w[root_w.size() - 1] == L'/')) {
        root_w.erase(root_w.size() - 1);
    }

    DWORD attrs = GetFileAttributesW(root_w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return std::vector<uint8_t>();
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 16 * 1024)) {
        return std::vector<uint8_t>();
    }

    bool ok = walk(root_w, root_w, zip);

    if (!ok) {
        mz_zip_writer_end(&zip);
        return std::vector<uint8_t>();
    }

    void* out_ptr = NULL;
    size_t out_size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &out_ptr, &out_size)) {
        mz_zip_writer_end(&zip);
        return std::vector<uint8_t>();
    }

    std::vector<uint8_t> result(out_size);
    std::memcpy(result.data(), out_ptr, out_size);
    mz_free(out_ptr);
    mz_zip_writer_end(&zip);
    return result;
}

} // namespace kmp
