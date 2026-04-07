// main.cpp — KenshiMP Launcher
//
// Deploys KenshiMP.dll to Kenshi's mods folder and launches the game.
// Requires RE_Kenshi (KenshiLib mod loader) to be installed.
//
// Flow:
//   1. Find Kenshi install (Steam registry or CLI arg)
//   2. Check RE_Kenshi is installed
//   3. Create mods/KenshiMP/ folder
//   4. Copy KenshiMP.dll into it
//   5. Launch kenshi_x64.exe
//   6. Wait for exit

#include <iostream>
#include <string>
#include <filesystem>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fs = std::filesystem;

static const char* DLL_NAME = "KenshiMP.dll";
static const char* MOD_FOLDER = "mods";
static const char* MOD_NAME = "KenshiMP";

// ---------------------------------------------------------------------------
// Find Kenshi install via Steam registry
// ---------------------------------------------------------------------------
static std::string find_kenshi_steam() {
    HKEY key;
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
// Check if RE_Kenshi (mod loader) is installed
// ---------------------------------------------------------------------------
static bool check_re_kenshi(const fs::path& kenshi_dir) {
    auto mods_dir = kenshi_dir / MOD_FOLDER;
    if (!fs::exists(mods_dir)) {
        std::cout << "Note: '" << MOD_FOLDER << "' folder not found. Creating it.\n";
        std::cout << "Make sure RE_Kenshi (KenshiLib mod loader) is installed!\n";
        std::cout << "Without it, KenshiMP.dll will not be loaded.\n\n";
    }
    return true;
}

// ---------------------------------------------------------------------------
// Deploy DLL to mods folder
// ---------------------------------------------------------------------------
static bool deploy_dll(const fs::path& kenshi_dir) {
    auto mod_dir = kenshi_dir / MOD_FOLDER / MOD_NAME;
    auto dst = mod_dir / DLL_NAME;

    // Create mod directory
    std::error_code ec;
    fs::create_directories(mod_dir, ec);
    if (ec) {
        std::cerr << "Error creating mod directory: " << ec.message() << "\n";
        return false;
    }

    // Find the DLL (next to launcher, or in parent, or in build output)
    fs::path src;
    fs::path candidates[] = {
        fs::path(DLL_NAME),
        fs::path("..") / DLL_NAME,
        fs::path("..") / "core" / "Release" / DLL_NAME,
        fs::path("Release") / DLL_NAME,
    };

    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            src = candidate;
            break;
        }
    }

    if (src.empty()) {
        if (fs::exists(dst)) {
            std::cout << "Using existing " << DLL_NAME << " in mod folder\n";
            return true;
        }
        std::cerr << "Error: " << DLL_NAME << " not found.\n";
        std::cerr << "Place it next to this launcher or build it first.\n";
        return false;
    }

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Error copying DLL: " << ec.message() << "\n";
        return false;
    }

    std::cout << "Deployed " << DLL_NAME << " to " << mod_dir << "\n";
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

    // Check for mod loader
    check_re_kenshi(kenshi_dir);

    // Deploy DLL
    if (!deploy_dll(kenshi_dir)) return 1;

    // Launch
    return launch_kenshi(kenshi_dir);
}
