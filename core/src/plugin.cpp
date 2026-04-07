// plugin.cpp — KenshiLib plugin entry point for KenshiMP
//
// RE_Kenshi mod loader calls startPlugin() when the mod is loaded.
// We hook the game's main loop via KenshiLib::AddHook to get
// per-frame callbacks on the game thread.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <core/Functions.h>
#include <OgreLogManager.h>

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
// Game loop hook
// ---------------------------------------------------------------------------
static void (*s_original_main_loop)(GameWorld* world, float time) = NULL;
static bool s_subsystems_initialized = false;

static void hooked_main_loop(GameWorld* world, float time) {
    // Call original game logic first
    if (s_original_main_loop) {
        s_original_main_loop(world, time);
    }

    // Defer subsystem init to first hook call (MyGUI not ready during startPlugin)
    if (!s_subsystems_initialized) {
        s_subsystems_initialized = true;
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Initialising subsystems...");
        kmp::client_init();
        kmp::npc_manager_init();
        kmp::player_sync_init();
        kmp::ui_init();
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Subsystems ready");
    }

    // Run multiplayer sync on the game thread
    kmp::player_sync_tick(time);
}

// ---------------------------------------------------------------------------
// Plugin entry point — called by RE_Kenshi mod loader
// ---------------------------------------------------------------------------
__declspec(dllexport) void startPlugin() {
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loading...");

    // Get the game's base address and compute the function address from RVA
    // RVA 0x7877A0 = GameWorld::mainLoop_GPUSensitiveStuff (from KenshiLib header)
    // Can't use GetRealAddress — the _NV_ stub is empty (zero bytes)
    uintptr_t game_base = (uintptr_t)GetModuleHandle(NULL);
    intptr_t func_addr = (intptr_t)(game_base + 0x7877A0);

    KenshiLib::HookStatus status = KenshiLib::AddHook(
        func_addr,
        &hooked_main_loop,
        &s_original_main_loop
    );

    if (status != KenshiLib::SUCCESS) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] FATAL: Failed to hook game loop!"
        );
        return;
    }

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loaded OK (game loop hooked)");
}
