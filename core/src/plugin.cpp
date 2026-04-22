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
#include "kmp_log.h"

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
    void ui_update_main_menu_button();
    void snapshot_uploader_glue_init();
    void snapshot_uploader_glue_shutdown();
    void server_browser_init();
    void server_browser_shutdown();
    void server_browser_tick(float dt);
}

// ---------------------------------------------------------------------------
// Game loop hook
// ---------------------------------------------------------------------------
static void (*s_original_main_loop)(GameWorld* world, float time) = NULL;
static bool s_subsystems_initialized = false;

// Separate hook for TitleScreen::update — the main game-loop hook doesn't fire
// until a GameWorld exists, so we'd never get a chance to attach UI to the
// title screen otherwise. Running on the render thread, MyGUI calls are safe.
static void (*s_original_title_update)(void* self) = NULL;
static bool s_title_ui_inited = false;

static void hooked_title_update(void* self) {
    if (s_original_title_update) s_original_title_update(self);

    // MyGUI is ready by the time TitleScreen::update runs. Init only the UI
    // subsystem here — client/npc_manager/player_sync are in-game-only and
    // would try to touch the world which doesn't exist yet.
    if (!s_title_ui_inited) {
        s_title_ui_inited = true;
        KMP_LOG("[KenshiMP] Title screen detected; initialising menu UI...");
        kmp::ui_init();
        kmp::server_browser_init();
    }
    kmp::ui_update_main_menu_button();
    // Tick the server browser too — its ping state machine needs frame
    // updates to drive ENet handshake + timeouts. player_sync_tick (the
    // in-game tick) doesn't fire on the title screen where the browser
    // lives, so we tick here. dt is approximate (title-screen hook doesn't
    // pass real dt); 1/60s is fine for network-grade timing.
    kmp::server_browser_tick(0.016f);
}

static void hooked_main_loop(GameWorld* world, float time) {
    // Call original game logic first
    if (s_original_main_loop) {
        s_original_main_loop(world, time);
    }

    // Defer subsystem init to first hook call (MyGUI not ready during startPlugin)
    if (!s_subsystems_initialized) {
        s_subsystems_initialized = true;
        KMP_LOG("[KenshiMP] Initialising subsystems...");
        kmp::client_init();
        kmp::npc_manager_init();
        kmp::player_sync_init();
        // ui_init may have already run from the TitleScreen hook — it's a
        // no-op when s_ui_initialized is true (see ui.cpp).
        if (!s_title_ui_inited) kmp::ui_init();
        kmp::snapshot_uploader_glue_init();
        kmp::server_browser_init();
        KMP_LOG("[KenshiMP] Subsystems ready");
    }

    // Run multiplayer sync on the game thread
    kmp::player_sync_tick(time);
}

// ---------------------------------------------------------------------------
// Plugin entry point — called by RE_Kenshi mod loader
// ---------------------------------------------------------------------------
__declspec(dllexport) void startPlugin() {
    kmp::KmpLog::get().init();
    KMP_LOG("[KenshiMP] Plugin loading...");

    // Get the address of the KenshiLib stub function via GetProcAddress
    // Can't use member function pointers — virtual ones contain vtable indices,
    // _NV_ ones are empty stubs. GetProcAddress gives us the actual stub address
    // which falls within FUNC_BEGIN/FUNC_END range.
    HMODULE klib = GetModuleHandleA("KenshiLib.dll");
    void* stub = (void*)GetProcAddress(klib,
        "?mainLoop_GPUSensitiveStuff@GameWorld@@UEAAXM@Z");

    if (!stub) {
        KMP_LOG(
            "[KenshiMP] FATAL: Could not find mainLoop_GPUSensitiveStuff in KenshiLib!");
        return;
    }

    intptr_t func_addr = KenshiLib::GetRealAddress(stub);

    KenshiLib::HookStatus status = KenshiLib::AddHook(
        func_addr,
        &hooked_main_loop,
        &s_original_main_loop
    );

    if (status != KenshiLib::SUCCESS) {
        KMP_LOG(
            "[KenshiMP] FATAL: Failed to hook game loop!"
        );
        return;
    }

    // Second hook: TitleScreen::update — runs every frame on the main menu,
    // before any GameWorld exists. Gives us a tick source to attach and drive
    // menu UI (the "Multiplayer" button).
    void* ts_stub = (void*)GetProcAddress(klib,
        "?update@TitleScreen@@UEAAXXZ");
    if (ts_stub) {
        intptr_t ts_addr = KenshiLib::GetRealAddress(ts_stub);
        KenshiLib::HookStatus ts_status = KenshiLib::AddHook(
            ts_addr,
            &hooked_title_update,
            &s_original_title_update
        );
        if (ts_status != KenshiLib::SUCCESS) {
            KMP_LOG("[KenshiMP] Warning: failed to hook TitleScreen::update (menu UI unavailable)");
        } else {
            KMP_LOG("[KenshiMP] Hooked TitleScreen::update");
        }
    } else {
        KMP_LOG("[KenshiMP] Warning: TitleScreen::update symbol not found");
    }

    KMP_LOG("[KenshiMP] Plugin loaded OK (game loop hooked)");
}
