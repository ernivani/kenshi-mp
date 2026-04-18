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
