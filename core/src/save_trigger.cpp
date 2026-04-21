#include "save_trigger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <kenshi/SaveFileSystem.h>

#include "kmp_log.h"

namespace kmp {

namespace {

static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                     static_cast<int>(w.size()),
                                     NULL, 0, NULL, NULL);
    std::string s(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], needed, NULL, NULL);
    return s;
}

static std::string documents_path() {
    wchar_t buf[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL,
                                    SHGFP_TYPE_CURRENT, buf))) {
        return std::string();
    }
    return wide_to_utf8(buf);
}

} // namespace

bool save_trigger_start(const std::string& slot_name) {
    SaveFileSystem* sfs = SaveFileSystem::getSingleton();
    if (!sfs) {
        KMP_LOG("[KenshiMP] save_trigger: SaveFileSystem singleton is null");
        return false;
    }
    std::string path = save_trigger_resolve_slot_path(slot_name);
    if (path.empty()) {
        KMP_LOG("[KenshiMP] save_trigger: could not resolve Documents path");
        return false;
    }
    bool ok = sfs->saveGame(path);
    KMP_LOG(std::string("[KenshiMP] save_trigger: saveGame('") + path
            + "') returned " + (ok ? "true" : "false"));
    return ok;
}

bool save_trigger_is_busy() {
    SaveFileSystem* sfs = SaveFileSystem::getSingleton();
    if (!sfs) return false;
    return sfs->busy();
}

std::string save_trigger_resolve_slot_path(const std::string& slot_name) {
    std::string docs = documents_path();
    if (docs.empty()) return std::string();
    return docs + "\\My Games\\Kenshi\\save\\" + slot_name;
}

} // namespace kmp
