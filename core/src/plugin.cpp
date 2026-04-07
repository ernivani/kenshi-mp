// plugin.cpp — KenshiLib plugin entry point for KenshiMP
//
// RE_Kenshi mod loader calls startPlugin() when the mod is loaded.
// We use Ogre::FrameListener for per-frame updates.
// Subsystem init is deferred to the first frame since MyGUI and
// game systems aren't ready during plugin load.

#include <OgreRoot.h>
#include <OgreFrameListener.h>
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
// Ogre FrameListener — called every frame by the rendering engine
// ---------------------------------------------------------------------------
class KenshiMPFrameListener : public Ogre::FrameListener {
public:
    bool frameRenderingQueued(const Ogre::FrameEvent& evt) override {
        // Defer subsystem init to first frame (MyGUI not ready during plugin load)
        if (!m_initialized) {
            m_initialized = true;
            Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Initialising subsystems...");
            kmp::client_init();
            kmp::npc_manager_init();
            kmp::player_sync_init();
            kmp::ui_init();
            Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Subsystems ready");
        }

        kmp::player_sync_tick(evt.timeSinceLastFrame);
        return true;
    }

private:
    bool m_initialized = false;
};

static KenshiMPFrameListener* s_listener = nullptr;

// ---------------------------------------------------------------------------
// Plugin entry point — called by RE_Kenshi mod loader
// ---------------------------------------------------------------------------
__declspec(dllexport) void startPlugin() {
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loading...");

    // Register frame listener — actual init happens on first frame
    s_listener = new KenshiMPFrameListener();
    Ogre::Root::getSingleton().addFrameListener(s_listener);

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin loaded OK");
}
