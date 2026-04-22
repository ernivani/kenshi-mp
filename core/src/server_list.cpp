#include "server_list.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

namespace kmp {

namespace {

struct Parser {
    const std::string& s;
    size_t i;
    Parser(const std::string& _s) : s(_s), i(0) {}
    void skip_ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool eof() { return i >= s.size(); }
    char peek() { return eof() ? '\0' : s[i]; }
    bool match(char c) { skip_ws(); if (eof() || s[i] != c) return false; ++i; return true; }
    bool parse_string(std::string& out) {
        skip_ws();
        if (!match('"')) return false;
        out.clear();
        while (!eof() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char esc = s[i + 1];
                if (esc == '"')       out += '"';
                else if (esc == '\\') out += '\\';
                else if (esc == 'n')  out += '\n';
                else if (esc == 't')  out += '\t';
                else if (esc == '/')  out += '/';
                else return false;
                i += 2;
            } else {
                out += s[i++];
            }
        }
        if (eof()) return false;
        ++i;
        return true;
    }
    bool parse_uint64(uint64_t& out) {
        skip_ws();
        if (eof() || !(s[i] >= '0' && s[i] <= '9')) return false;
        out = 0;
        while (!eof() && s[i] >= '0' && s[i] <= '9') {
            out = out * 10 + static_cast<uint64_t>(s[i] - '0');
            ++i;
        }
        return true;
    }
};

static std::string escape_string(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static bool parse_entry(Parser& p, ServerEntry& e) {
    if (!p.match('{')) return false;
    bool first = true;
    while (true) {
        p.skip_ws();
        if (p.peek() == '}') { ++p.i; return true; }
        if (!first) { if (!p.match(',')) return false; }
        first = false;
        std::string key;
        if (!p.parse_string(key)) return false;
        if (!p.match(':')) return false;
        if (key == "id")                { if (!p.parse_string(e.id)) return false; }
        else if (key == "name")         { if (!p.parse_string(e.name)) return false; }
        else if (key == "address")      { if (!p.parse_string(e.address)) return false; }
        else if (key == "password")     { if (!p.parse_string(e.password)) return false; }
        else if (key == "port")         { uint64_t v; if (!p.parse_uint64(v)) return false; e.port = static_cast<uint16_t>(v); }
        else if (key == "last_joined_ms") { uint64_t v; if (!p.parse_uint64(v)) return false; e.last_joined_ms = v; }
        else { return false; }
    }
}

static bool parse_document(const std::string& text, std::vector<ServerEntry>& out) {
    Parser p(text);
    if (!p.match('{')) return false;
    bool saw_version = false;
    bool saw_servers = false;
    bool first = true;
    while (true) {
        p.skip_ws();
        if (p.peek() == '}') { ++p.i; break; }
        if (!first) { if (!p.match(',')) return false; }
        first = false;
        std::string key;
        if (!p.parse_string(key)) return false;
        if (!p.match(':')) return false;
        if (key == "version") {
            uint64_t v; if (!p.parse_uint64(v)) return false;
            if (v != 1) return false;
            saw_version = true;
        } else if (key == "servers") {
            if (!p.match('[')) return false;
            bool arr_first = true;
            while (true) {
                p.skip_ws();
                if (p.peek() == ']') { ++p.i; break; }
                if (!arr_first) { if (!p.match(',')) return false; }
                arr_first = false;
                ServerEntry e;
                if (!parse_entry(p, e)) return false;
                out.push_back(e);
            }
            saw_servers = true;
        } else {
            return false;
        }
    }
    return saw_version && saw_servers;
}

// Win32 helpers — no std::filesystem (v100 lacks it)
static bool win32_create_directories(const std::string& path) {
    // Walk the path and create each component
    std::string p = path;
    // Normalize backslashes
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i] == '/') p[i] = '\\';
    // Create directories level by level
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\' && i > 0) {
            std::string sub = p.substr(0, i);
            CreateDirectoryA(sub.c_str(), NULL); // ignore errors (may already exist)
        }
    }
    return CreateDirectoryA(p.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool win32_rename(const std::string& src, const std::string& dst) {
    return MoveFileExA(src.c_str(), dst.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static bool win32_remove(const std::string& path) {
    return DeleteFileA(path.c_str()) != 0;
}

static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                NULL, 0, NULL, NULL);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &s[0], n, NULL, NULL);
    return s;
}

// Extract parent directory from a path string
static std::string parent_path(const std::string& path) {
    std::string p = path;
    // Find last separator
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    return p.substr(0, pos);
}

} // namespace

bool server_list_load_from(const std::string& path, std::vector<ServerEntry>& out) {
    out.clear();
    std::ifstream f(path.c_str());
    if (!f.is_open()) return false;

    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();
    f.close();

    std::vector<ServerEntry> parsed;
    if (!parse_document(text, parsed)) {
        std::string dst = path + ".corrupt-" + std::to_string(static_cast<long long>(std::time(NULL)));
        win32_rename(path, dst);
        return false;
    }
    out = parsed;
    return true;
}

bool server_list_save_to(const std::string& path, const std::vector<ServerEntry>& in) {
    std::string parent = parent_path(path);
    if (!parent.empty()) {
        win32_create_directories(parent);
    }

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp.c_str());
        if (!f.is_open()) return false;
        f << "{\n  \"version\": 1,\n  \"servers\": [";
        for (size_t i = 0; i < in.size(); ++i) {
            const ServerEntry& e = in[i];
            f << (i > 0 ? ",\n" : "\n")
              << "    {\n"
              << "      \"id\": \""             << escape_string(e.id) << "\",\n"
              << "      \"name\": \""           << escape_string(e.name) << "\",\n"
              << "      \"address\": \""        << escape_string(e.address) << "\",\n"
              << "      \"port\": "             << static_cast<int>(e.port) << ",\n"
              << "      \"password\": \""       << escape_string(e.password) << "\",\n"
              << "      \"last_joined_ms\": "   << e.last_joined_ms << "\n"
              << "    }";
        }
        f << (in.empty() ? "]\n}\n" : "\n  ]\n}\n");
        if (!f.good()) return false;
    }

    win32_remove(path);
    return win32_rename(tmp, path);
}

std::string server_list_default_path() {
    wchar_t buf[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL,
                                    SHGFP_TYPE_CURRENT, buf))) {
        return std::string();
    }
    return wide_to_utf8(buf) + "\\My Games\\Kenshi\\KenshiMP\\servers.json";
}

bool server_list_load(std::vector<ServerEntry>& out) {
    std::string path = server_list_default_path();
    if (path.empty()) return false;
    return server_list_load_from(path, out);
}

bool server_list_save(const std::vector<ServerEntry>& in) {
    std::string path = server_list_default_path();
    if (path.empty()) return false;
    return server_list_save_to(path, in);
}

std::string server_list_new_id() {
    static bool seeded = false;
    if (!seeded) { std::srand(static_cast<unsigned>(std::time(NULL)) ^ GetCurrentProcessId()); seeded = true; }
    char buf[9];
    for (int i = 0; i < 8; ++i) {
        int v = std::rand() & 0xF;
        buf[i] = (v < 10) ? ('0' + v) : ('a' + v - 10);
    }
    buf[8] = '\0';
    return std::string(buf);
}

} // namespace kmp
