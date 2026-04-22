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

// Tick count when the last save_trigger_start() was called. Used by
// save_trigger_is_busy() to treat the first ~few seconds as "still busy"
// even if SaveFileSystem::busy() hasn't flipped true yet — Kenshi's
// SaveManager::save() just queues a signal; the actual worker write
// doesn't begin until SaveManager::execute() runs on the next tick, so
// there's a gap where busy() is transiently false.
static ULONGLONG s_save_started_ms = 0;

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

    // Tried sm->save(slot, autosave=true) — it just sets a signal and
    // relies on SaveManager::execute() running later on the game thread.
    // In practice execute() never dispatches our signal (probably because
    // the menu context or some precondition isn't met), so the save never
    // actually happens. Go straight to saveGame(location, name) — the
    // synchronous internal call that the signal path dispatches to anyway.
    const std::string& loc = sm->userSavePath.empty() ? sm->localSavePath
                                                      : sm->userSavePath;
    int rc = sm->saveGame(loc, slot_name);
    s_save_started_ms = GetTickCount64();
    KMP_LOG(std::string("[KenshiMP] save_trigger: SaveManager::saveGame(loc='")
            + loc + "', name='" + slot_name + "') returned "
            + std::to_string(static_cast<long long>(rc)));
    return true;
}

bool save_trigger_is_busy() {
    // SaveManager::save() only *queues*: the actual write happens on the
    // next tick when Kenshi's main loop calls SaveManager::execute(), which
    // dispatches to SaveFileSystem on the worker thread.
    //
    // Two races to guard against:
    //   1. Query before execute() dispatches → busy()=false but save hasn't
    //      started yet. We'd transition to ZIP with stale files on disk.
    //   2. Query during the worker write → busy()=true. This is the normal
    //      case and we stay in WAIT_SAVE.
    //
    // Solution: report busy for at least kGraceMs after start() regardless
    // of what busy() says, then rely on busy() alone once grace elapses.
    // Empirically Kenshi's save takes 0.5-3s on a mid-game save, so 4s is
    // safe without noticeably delaying the happy path.
    const ULONGLONG kGraceMs = 4000;
    ULONGLONG now_ms = GetTickCount64();
    bool in_grace = (s_save_started_ms > 0) &&
                    (now_ms - s_save_started_ms < kGraceMs);

    SaveFileSystem* sfs = SaveFileSystem::getSingleton();
    bool worker_busy = sfs ? sfs->busy() : false;

    return in_grace || worker_busy;
}

std::string save_trigger_resolve_slot_path(const std::string& slot_name) {
    // Ask SaveManager where it actually writes saves. Kenshi's save root
    // is NOT always <Documents>\My Games\Kenshi\save — the GOG build and
    // some configurations use <AppData>\Local\kenshi\save instead. The
    // SaveManager knows the real path via its userSavePath / localSavePath
    // members, populated at init by Kenshi.
    SaveManager* sm = SaveManager::getSingleton();
    if (sm) {
        const std::string& root = sm->userSavePath.empty() ? sm->localSavePath
                                                           : sm->userSavePath;
        if (!root.empty()) {
            // userSavePath usually has a trailing separator; tolerate both.
            std::string base = root;
            if (!base.empty() && base.back() != '\\' && base.back() != '/') {
                base += '\\';
            }
            return base + slot_name;
        }
    }
    // Fallback: <Documents>\My Games\Kenshi\save\<slot> (Steam default).
    std::string docs = documents_path();
    if (docs.empty()) return std::string();
    return docs + "\\My Games\\Kenshi\\save\\" + slot_name;
}

} // namespace kmp
