#include "load_trigger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>

#include <kenshi/SaveFileSystem.h>
#include <kenshi/SaveManager.h>

#include "kmp_log.h"
#include "save_trigger.h"

namespace kmp {

static ULONGLONG s_load_started_ms = 0;

bool load_trigger_start(const std::string& slot_name) {
    SaveManager* sm = SaveManager::getSingleton();
    if (!sm) {
        KMP_LOG("[KenshiMP] load_trigger: SaveManager singleton null");
        return false;
    }
    // Same location resolution as save_trigger — userSavePath or localSavePath.
    const std::string& loc = sm->userSavePath.empty() ? sm->localSavePath
                                                      : sm->userSavePath;
    int rc = sm->loadGame(loc, slot_name);
    s_load_started_ms = GetTickCount64();
    char msg[256];
    _snprintf(msg, sizeof(msg),
        "[KenshiMP] load_trigger: loadGame(loc='%s', name='%s') returned %d",
        loc.c_str(), slot_name.c_str(), rc);
    KMP_LOG(msg);
    // Return non-negative means queued; negative means synchronous refusal.
    return rc >= 0;
}

bool load_trigger_is_busy() {
    const ULONGLONG kGraceMs = 4000;
    ULONGLONG now = GetTickCount64();
    bool in_grace = (s_load_started_ms > 0) &&
                    (now - s_load_started_ms < kGraceMs);
    SaveFileSystem* sfs = SaveFileSystem::getSingleton();
    bool worker_busy = sfs ? sfs->busy() : false;
    return in_grace || worker_busy;
}

std::string load_trigger_resolve_slot_path(const std::string& slot_name) {
    return save_trigger_resolve_slot_path(slot_name);
}

} // namespace kmp
