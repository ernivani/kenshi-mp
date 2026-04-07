// main.cpp — KenshiMP Launcher / Injector
//
// This launcher:
//   1. Finds the Kenshi install directory (Steam registry or manual path)
//   2. Backs up Plugins_x64.cfg
//   3. Appends "Plugin=KenshiMP" to the config
//   4. Copies KenshiMP.dll to the Kenshi directory if needed
//   5. Launches kenshi_x64.exe
//   6. On exit, restores the original Plugins_x64.cfg

#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Find Kenshi install via Steam registry
// ---------------------------------------------------------------------------
static std::string find_kenshi_steam() {
    HKEY key;
    // Kenshi Steam App ID: 233860
    const char* reg_path = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 233860";

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, reg_path, 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        char buffer[MAX_PATH];
        DWORD size = sizeof(buffer);
        if (RegQueryValueExA(key, "InstallLocation", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS) {
            RegCloseKey(key);
            return std::string(buffer);
        }
        RegCloseKey(key);
    }
    return "";
}

// ---------------------------------------------------------------------------
// Plugin config management
// ---------------------------------------------------------------------------
static const char* PLUGIN_CFG    = "Plugins_x64.cfg";
static const char* PLUGIN_BACKUP = "Plugins_x64.cfg.kmp_backup";
static const char* PLUGIN_LINE   = "Plugin=KenshiMP";
static const char* DLL_NAME      = "KenshiMP.dll";

static bool config_has_plugin(const fs::path& cfg_path) {
    std::ifstream file(cfg_path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.find(PLUGIN_LINE) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool backup_config(const fs::path& kenshi_dir) {
    auto cfg = kenshi_dir / PLUGIN_CFG;
    auto bak = kenshi_dir / PLUGIN_BACKUP;

    if (!fs::exists(cfg)) {
        std::cerr << "Error: " << cfg << " not found\n";
        return false;
    }

    std::error_code ec;
    fs::copy_file(cfg, bak, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Error backing up config: " << ec.message() << "\n";
        return false;
    }
    return true;
}

static bool patch_config(const fs::path& kenshi_dir) {
    auto cfg = kenshi_dir / PLUGIN_CFG;

    if (config_has_plugin(cfg)) {
        std::cout << "Plugin already in config, skipping patch\n";
        return true;
    }

    std::ofstream file(cfg, std::ios::app);
    if (!file.is_open()) {
        std::cerr << "Error: cannot open " << cfg << " for writing\n";
        return false;
    }

    file << "\n" << PLUGIN_LINE << "\n";
    std::cout << "Patched " << PLUGIN_CFG << "\n";
    return true;
}

static void restore_config(const fs::path& kenshi_dir) {
    auto cfg = kenshi_dir / PLUGIN_CFG;
    auto bak = kenshi_dir / PLUGIN_BACKUP;

    if (fs::exists(bak)) {
        std::error_code ec;
        fs::copy_file(bak, cfg, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            fs::remove(bak, ec);
            std::cout << "Restored original " << PLUGIN_CFG << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Copy DLL if needed
// ---------------------------------------------------------------------------
static bool ensure_dll(const fs::path& kenshi_dir) {
    auto dst = kenshi_dir / DLL_NAME;

    // Check if DLL exists next to the launcher
    auto src = fs::path(DLL_NAME);
    if (!fs::exists(src)) {
        // Try in parent directory or build output
        src = fs::path("..") / DLL_NAME;
    }

    if (!fs::exists(src)) {
        if (fs::exists(dst)) {
            std::cout << "Using existing " << DLL_NAME << " in Kenshi directory\n";
            return true;
        }
        std::cerr << "Error: " << DLL_NAME << " not found\n";
        return false;
    }

    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Error copying DLL: " << ec.message() << "\n";
        return false;
    }

    std::cout << "Copied " << DLL_NAME << " to Kenshi directory\n";
    return true;
}

// ---------------------------------------------------------------------------
// Launch Kenshi
// ---------------------------------------------------------------------------
static int launch_kenshi(const fs::path& kenshi_dir) {
    auto exe = kenshi_dir / "kenshi_x64.exe";
    if (!fs::exists(exe)) {
        std::cerr << "Error: " << exe << " not found\n";
        return 1;
    }

    std::cout << "Launching " << exe << "...\n";

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::string exe_str = exe.string();
    std::string dir_str = kenshi_dir.string();

    if (!CreateProcessA(
            exe_str.c_str(),
            nullptr,
            nullptr, nullptr,
            FALSE, 0,
            nullptr,
            dir_str.c_str(),
            &si, &pi)) {
        std::cerr << "Error: CreateProcess failed (" << GetLastError() << ")\n";
        return 1;
    }

    std::cout << "Kenshi started (PID: " << pi.dwProcessId << ")\n";
    std::cout << "Waiting for Kenshi to exit...\n";

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::cout << "Kenshi exited with code " << exit_code << "\n";
    return static_cast<int>(exit_code);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::cout << "=== KenshiMP Launcher ===\n\n";

    // Determine Kenshi directory
    std::string kenshi_path;

    if (argc > 1) {
        kenshi_path = argv[1];
    } else {
        kenshi_path = find_kenshi_steam();
        if (kenshi_path.empty()) {
            std::cerr << "Could not find Kenshi install directory.\n";
            std::cerr << "Usage: " << argv[0] << " <path-to-kenshi>\n";
            return 1;
        }
    }

    fs::path kenshi_dir(kenshi_path);
    std::cout << "Kenshi directory: " << kenshi_dir << "\n";

    if (!fs::exists(kenshi_dir / "kenshi_x64.exe")) {
        std::cerr << "Error: kenshi_x64.exe not found in " << kenshi_dir << "\n";
        return 1;
    }

    // Step 1: Backup config
    if (!backup_config(kenshi_dir)) return 1;

    // Step 2: Copy DLL
    if (!ensure_dll(kenshi_dir)) {
        restore_config(kenshi_dir);
        return 1;
    }

    // Step 3: Patch config
    if (!patch_config(kenshi_dir)) {
        restore_config(kenshi_dir);
        return 1;
    }

    // Step 4: Launch and wait
    int result = launch_kenshi(kenshi_dir);

    // Step 5: Restore config
    restore_config(kenshi_dir);

    return result;
}
