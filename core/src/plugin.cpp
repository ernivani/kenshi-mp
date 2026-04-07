// plugin.cpp — KenshiLib plugin entry point for KenshiMP
//
// RE_Kenshi mod loader calls startPlugin() when the mod is loaded.
// We run our sync loop on a background thread since Ogre::FrameListener
// doesn't work with Kenshi's custom game loop.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <OgreLogManager.h>

// Forward declarations for our subsystems
namespace kmp {
    void client_init();
    void client_shutdown();
    void player_sync_init();
    void player_sync_shutdown();
    void player_sync_tick(float dt);
    void npc_manager_init();
    void npc_manager_shutdown();
    void ui_init();
    void ui_shutdown();
}

// ---------------------------------------------------------------------------
// Background thread — runs our sync loop at ~20Hz
// ---------------------------------------------------------------------------
static volatile bool s_running = false;
static HANDLE s_thread = nullptr;

static DWORD WINAPI sync_thread(LPVOID) {
    // Wait a few seconds for the game to fully initialize
    Sleep(5000);

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Sync thread starting...");

    kmp::client_init();
    kmp::npc_manager_init();
    kmp::player_sync_init();
    kmp::ui_init();

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Subsystems ready, entering loop");

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    while (s_running) {
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart);
        last = now;

        kmp::player_sync_tick(dt);

        // Sleep ~50ms (20Hz) but use shorter sleep for responsive hotkeys
        Sleep(16);
    }

    kmp::ui_shutdown();
    kmp::player_sync_shutdown();
    kmp::npc_manager_shutdown();
    kmp::client_shutdown();

    return 0;
}

// ---------------------------------------------------------------------------
// Plugin entry point — called by RE_Kenshi mod loader
// ---------------------------------------------------------------------------
__declspec(dllexport) void startPlugin() {
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loading...");

    s_running = true;
    s_thread = CreateThread(nullptr, 0, sync_thread, nullptr, 0, nullptr);

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loaded OK (sync thread started)");
}
