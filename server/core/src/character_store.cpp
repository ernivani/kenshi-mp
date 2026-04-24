#include "character_store.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

namespace kmp {

namespace {

static std::filesystem::path s_dir;

static bool sanitize_uuid(const std::string& uuid, std::string& out) {
    // Reject empty + any path-separator characters so we can't be tricked
    // into writing outside server_data/characters/.
    if (uuid.empty()) return false;
    out.clear();
    out.reserve(uuid.size());
    for (char c : uuid) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == '.') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return !out.empty();
}

static std::filesystem::path blob_path(const std::string& safe) {
    return s_dir / (safe + ".dat");
}

} // namespace

void character_store_init() {
    s_dir = std::filesystem::current_path() / "server_data" / "characters";
    std::error_code ec;
    std::filesystem::create_directories(s_dir, ec);
    if (ec) {
        spdlog::warn("character_store: failed to create {} : {}",
                     s_dir.string(), ec.message());
    } else {
        spdlog::info("character_store: ready at {}", s_dir.string());
    }
}

bool character_store_get(const std::string& uuid, std::vector<uint8_t>& out) {
    std::string safe;
    if (!sanitize_uuid(uuid, safe)) return false;
    auto path = blob_path(safe);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    std::streamsize size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(out.data()), size)) {
        out.clear();
        return false;
    }
    spdlog::info("character_store: loaded {} bytes for uuid={}",
                 out.size(), uuid);
    return true;
}

void character_store_set(const std::string& uuid,
                         const uint8_t* blob, size_t len) {
    std::string safe;
    if (!sanitize_uuid(uuid, safe)) {
        spdlog::warn("character_store: rejected empty/invalid uuid");
        return;
    }
    if (!blob || len == 0) {
        spdlog::warn("character_store: rejected empty blob for uuid={}", uuid);
        return;
    }
    auto path = blob_path(safe);
    // Write to .tmp then rename so readers never see a half-written file.
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            spdlog::error("character_store: could not open {} for writing",
                          tmp.string());
            return;
        }
        f.write(reinterpret_cast<const char*>(blob),
                static_cast<std::streamsize>(len));
        if (!f.good()) {
            spdlog::error("character_store: write failed for uuid={}", uuid);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        spdlog::error("character_store: rename failed {} -> {}: {}",
                      tmp.string(), path.string(), ec.message());
        return;
    }
    spdlog::info("character_store: saved {} bytes for uuid={}", len, uuid);
}

} // namespace kmp
