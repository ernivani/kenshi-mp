// game_hooks.cpp — KenshiLib accessor wrappers
//
// All functions run on the game thread (via AddHook), so no
// thread-safety guards are needed.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectFactory.h>

#include <vector>

#include "kmp_log.h"

namespace kmp {

Character* game_get_player_character() {
    if (!ou) return NULL;
    if (!ou->player) return NULL;

    const lektor<Character*>& chars = ou->player->getAllPlayerCharacters();
    if (chars.count <= 0) return NULL;

    return chars.stuff[0];
}

RootObjectFactory* game_get_factory() {
    if (!ou) return NULL;
    return ou->theFactory;
}

bool game_is_world_loaded() {
    if (!ou) return false;
    if (!ou->player) return false;

    const lektor<Character*>& chars = ou->player->getAllPlayerCharacters();
    return chars.count > 0;
}

GameWorld* game_get_world() {
    return ou;
}

extern bool npc_manager_is_player_npc(Character* ch);

// Serialize the local PlayerInterface squad into a byte blob by
// saving to a temp file (Kenshi's GameData API is file-based) then
// reading the file back. Temp file is deleted on the way out. Returns
// empty vector on failure.
static std::string temp_char_path() {
    char tmp[MAX_PATH] = {0};
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string dir = (n > 0 && n < MAX_PATH) ? std::string(tmp) : std::string(".\\");
    return dir + "KenshiMP_char_" +
        std::to_string(static_cast<unsigned long long>(GetCurrentProcessId())) +
        ".dat";
}

std::vector<uint8_t> game_serialize_player_state() {
    std::vector<uint8_t> out;
    if (!ou || !ou->player) return out;

    std::string path = temp_char_path();
    GameDataCopyStandalone gd;
    gd.initialise(CHARACTER, false);
    ou->player->serialise(&gd);
    bool saved = gd.saveToFile(path);
    gd.destroy();
    if (!saved) {
        DeleteFileA(path.c_str());
        return out;
    }

    FILE* f = NULL;
    if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            out.resize(static_cast<size_t>(sz));
            size_t got = fread(out.data(), 1, out.size(), f);
            if (got != out.size()) out.clear();
        }
        fclose(f);
    }
    DeleteFileA(path.c_str());
    return out;
}

// Serialize just one Character's appearance blob (race, face, hair,
// body, gear) via Kenshi's per-char API. Returns empty vector on
// failure. Safe to call on a live local character.
std::vector<uint8_t> game_serialize_character_appearance(Character* ch) {
    std::vector<uint8_t> out;
    if (!ch) return out;
    GameDataCopyStandalone* gd = ch->getAppearanceData();
    if (!gd) return out;

    std::string path = temp_char_path() + ".app";
    if (!gd->saveToFile(path)) return out;

    FILE* f = NULL;
    if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            out.resize(static_cast<size_t>(sz));
            size_t got = fread(out.data(), 1, out.size(), f);
            if (got != out.size()) out.clear();
        }
        fclose(f);
    }
    DeleteFileA(path.c_str());
    return out;
}

// Inverse: write the blob to a temp file, call GameData::loadFromFile
// + PlayerInterface::loadFromSerialise. Returns true on success.
bool game_apply_player_state(const uint8_t* blob, size_t len) {
    if (!ou || !ou->player) return false;
    if (!blob || len == 0) return false;

    std::string path = temp_char_path();
    FILE* f = NULL;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    size_t put = fwrite(blob, 1, len, f);
    fclose(f);
    if (put != len) { DeleteFileA(path.c_str()); return false; }

    GameDataCopyStandalone gd;
    bool ok = gd.loadFromFile(path, CHARACTER);
    if (ok) ou->player->loadFromSerialise(&gd);
    gd.destroy();
    DeleteFileA(path.c_str());
    return ok;
}

// Spawn a blank character in the local player faction. Used after a
// snapshot join so the joiner has something to control. Tries a list of
// common Kenshi character template names; falls back to random if none
// match. Logs the name of the matched template (or "random") so we can
// discover what the real names are in this Kenshi install.
Character* game_spawn_joiner_character_and_edit() {
    if (!ou) return NULL;
    if (!ou->theFactory) return NULL;
    if (!ou->player) return NULL;

    Faction* faction = ou->player->getFaction();
    if (!faction) return NULL;

    // Dump up to 40 CHARACTER templates available, first time this runs,
    // so we can identify valid starter names from the log.
    static bool s_dumped = false;
    if (!s_dumped) {
        s_dumped = true;
        lektor<GameData*> list;
        ou->gamedata.getDataOfType(list, CHARACTER);
        char hdr[96];
        _snprintf(hdr, sizeof(hdr),
            "[KenshiMP] CHARACTER GameData count=%d (dumping first 40):",
            list.count);
        KMP_LOG(hdr);
        int lim = list.count < 40 ? list.count : 40;
        for (int i = 0; i < lim; ++i) {
            if (list.stuff[i]) {
                GameData* d = list.stuff[i];
                KMP_LOG(std::string("[KenshiMP]   CHARACTER: ") + d->name);
            }
        }
    }

    GameData* tmpl = NULL;
    const char* candidates[] = {
        "Wanderer",          // starter preset — verified present in this install
        "UC start",
        "Greenlander", "greenlander",
        NULL
    };
    const char* matched = NULL;
    for (int i = 0; candidates[i]; ++i) {
        tmpl = ou->gamedata.getDataByName(std::string(candidates[i]), CHARACTER);
        if (tmpl) { matched = candidates[i]; break; }
    }
    KMP_LOG(std::string("[KenshiMP] joiner char template = ") +
        (matched ? matched : "(none — fallback random)"));

    Ogre::Vector3 pos = ou->getCameraCenter();
    RootObjectBase* obj = ou->theFactory->createRandomCharacter(
        faction, pos, NULL, tmpl, NULL, 0.0f);
    Character* ch = dynamic_cast<Character*>(obj);
    // Investigation: re-enable auto-open while disabling CHARACTER_UPLOAD
    // periodic send (see player_sync.cpp). Hypothesis B: serialising
    // PlayerInterface mid-game corrupts Character state, cascading into
    // host crash on subsequent PlayerState receive.
    if (ch) {
        extern void char_editor_open_deferred(Character* ch);
        char_editor_open_deferred(ch);
    }
    return ch;
}

// Destroy every character in the local PlayerInterface's squad. Used after
// a snapshot join so the joiner doesn't inherit the host's squad. Skips
// characters that are registered as remote players — those are spawned
// by the server's SpawnNPC path into the player faction (fallback when
// getEmptyFaction returns null) and must NOT be destroyed here.
// Returns the number of characters actually destroyed.
int game_destroy_inherited_player_squad() {
    if (!ou) return 0;
    if (!ou->player) return 0;

    const lektor<Character*>& chars = ou->player->getAllPlayerCharacters();
    std::vector<Character*> copy;
    copy.reserve(chars.count);
    for (int i = 0; i < chars.count; ++i) {
        if (chars.stuff[i]) copy.push_back(chars.stuff[i]);
    }
    int n = 0;
    for (size_t i = 0; i < copy.size(); ++i) {
        if (npc_manager_is_player_npc(copy[i])) continue;
        if (ou->destroy(copy[i], false, "kmp: clear inherited squad")) {
            ++n;
        }
    }
    return n;
}

} // namespace kmp
