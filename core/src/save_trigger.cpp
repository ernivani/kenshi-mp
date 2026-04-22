#include "save_trigger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <kenshi/SaveFileSystem.h>
#include <kenshi/SaveManager.h>

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
    // Use the high-level SaveManager::save() entry point — same one the
    // in-game Save menu calls. It populates save.xml, quick.save, itemKey,
    // characterKey, etc. and queues the actual file write on Kenshi's save
    // worker thread. Polling SaveFileSystem::busy() tells us when it's done.
    //
    // NB: we used to call SaveFileSystem::saveGame(path) directly, but that's
    // the low-level file-mover — it skips all the preparation SaveManager
    // does, resulting in partial saves and a "Could not find the save folder"
    // dialog from Kenshi.
    SaveManager* sm = SaveManager::getSingleton();
    if (!sm) {
        KMP_LOG("[KenshiMP] save_trigger: SaveManager singleton is null");
        return false;
    }
    sm->save(slot_name, /*autosave=*/true);
    KMP_LOG(std::string("[KenshiMP] save_trigger: SaveManager::save('")
            + slot_name + "') queued");
    return true;
}

bool save_trigger_is_busy() {
    // SaveManager::save() only *queues*: the actual write happens on the
    // next tick when Kenshi's main loop calls SaveManager::execute(), which
    // dispatches to SaveFileSystem on the worker thread. To avoid
    // transitioning to ZIP before the save has even started, report busy if
    // EITHER the SaveManager has a pending signal OR the worker is writing.
    SaveManager* sm = SaveManager::getSingleton();
    if (sm && sm->signal != 0) return true;
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
