#include "client_identity.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include <windows.h>
#include <shlobj.h>
#include <rpc.h>

#pragma comment(lib, "rpcrt4.lib")

static std::string s_uuid;

static std::string appdata_dir() {
    char path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        std::string dir = std::string(path) + "\\KenshiMP";
        CreateDirectoryA(dir.c_str(), NULL);  // idempotent
        return dir;
    }
    return ".";
}

static std::string uuid_file() {
    return appdata_dir() + "\\client_uuid.txt";
}

static std::string generate_uuid_string() {
    UUID id;
    UuidCreate(&id);
    RPC_CSTR str = NULL;
    if (UuidToStringA(&id, &str) == RPC_S_OK && str) {
        std::string out((char*)str);
        RpcStringFreeA(&str);
        return out;
    }
    return "00000000-0000-0000-0000-000000000000";
}

const char* client_identity_get_uuid() {
    if (!s_uuid.empty()) return s_uuid.c_str();

    std::string path = uuid_file();
    {
        std::ifstream f(path.c_str());
        if (f.is_open()) {
            std::string line;
            std::getline(f, line);
            // Trim whitespace/newlines.
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                                     line.back() == ' '  || line.back() == '\t'))
                line.pop_back();
            if (line.size() >= 32 && line.size() < 64) {
                s_uuid = line;
                return s_uuid.c_str();
            }
        }
    }

    s_uuid = generate_uuid_string();
    std::ofstream out(path.c_str());
    if (out.is_open()) out << s_uuid;
    return s_uuid.c_str();
}

// ---------------------------------------------------------------------------
// Character identity (name + model)
// ---------------------------------------------------------------------------
static std::string s_char_name;
static std::string s_char_model;
static bool        s_char_loaded = false;
static bool        s_char_has_custom = false;

static std::string character_file() {
    return appdata_dir() + "\\character.txt";
}

static void load_character_once() {
    if (s_char_loaded) return;
    s_char_loaded = true;
    std::ifstream f(character_file().c_str());
    if (!f.is_open()) return;
    std::string name_line, model_line;
    std::getline(f, name_line);
    std::getline(f, model_line);
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t'))
            s.pop_back();
    };
    trim(name_line); trim(model_line);
    if (!name_line.empty())  s_char_name  = name_line;
    if (!model_line.empty()) s_char_model = model_line;
    if (!name_line.empty() || !model_line.empty()) s_char_has_custom = true;
}

static void save_character() {
    std::ofstream out(character_file().c_str());
    if (!out.is_open()) return;
    out << s_char_name << "\n" << s_char_model << "\n";
}

const std::string& client_identity_get_name() {
    load_character_once();
    if (s_char_name.empty()) {
        static const std::string kDefault = "Wanderer";
        return kDefault;
    }
    return s_char_name;
}

const std::string& client_identity_get_model() {
    load_character_once();
    if (s_char_model.empty()) {
        // "Wanderer" exists as a Kenshi CHARACTER GameData (verified via
        // ou->gamedata.getDataByName); "greenlander" does not — it's only
        // a race. Sending "Wanderer" makes the host spawn the joiner's
        // remote NPC with a proper template (tmpl=Wanderer in logs)
        // instead of falling back to random.
        static const std::string kDefault = "Wanderer";
        return kDefault;
    }
    return s_char_model;
}

void client_identity_set_name(const std::string& name) {
    load_character_once();
    if (name.empty()) return;
    s_char_name = name;
    s_char_has_custom = true;
    save_character();
}

void client_identity_set_model(const std::string& model) {
    load_character_once();
    if (model.empty()) return;
    s_char_model = model;
    s_char_has_custom = true;
    save_character();
}

bool client_identity_has_custom() {
    load_character_once();
    return s_char_has_custom;
}
