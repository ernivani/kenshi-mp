// plugin.cpp — KenshiLib plugin entry point for KenshiMP
//
// RE_Kenshi mod loader calls startPlugin() when the mod is loaded.
// We hook the game's main render loop to run our sync every frame.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <core/Functions.h>
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
// Game loop hook
// ---------------------------------------------------------------------------
// Original function pointer — filled by KenshiLib::AddHook
static void (*s_original_main_loop)(GameWorld* world, float time) = nullptr;

static void hooked_main_loop(GameWorld* world, float time) {
    // Run the original game logic first
    if (s_original_main_loop) {
        s_original_main_loop(world, time);
    }

    // Then run our multiplayer sync
    kmp::player_sync_tick(time);
}

// ---------------------------------------------------------------------------
// Plugin entry point — called by RE_Kenshi mod loader
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void startPlugin() {
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loading...");

    // Hook the game's main render loop
    // GameWorld::mainLoop_GPUSensitiveStuff is called every frame with delta time
    auto status = KenshiLib::AddHook(
        reinterpret_cast<void*>(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff)
        ),
        reinterpret_cast<void*>(&hooked_main_loop),
        reinterpret_cast<void**>(&s_original_main_loop)
    );

    if (status != HookStatus::SUCCESS) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] FATAL: Failed to hook game loop!"
        );
        return;
    }

    // Init subsystems
    kmp::client_init();
    kmp::npc_manager_init();
    kmp::player_sync_init();
    kmp::ui_init();

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loaded OK");
}
