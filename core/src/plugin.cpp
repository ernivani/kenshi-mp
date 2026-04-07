// plugin.cpp — Ogre3D plugin entry point for KenshiMP
//
// Kenshi loads plugins listed in Plugins_x64.cfg. Each plugin must export
// dllStartPlugin / dllStopPlugin and provide an Ogre::Plugin implementation.

#include <OgrePlugin.h>
#include <OgreRoot.h>
#include <OgreRenderWindow.h>
#include <OgreLogManager.h>

// Forward declarations for our subsystems
namespace kmp {
    void client_init();
    void client_shutdown();
    void game_hooks_install();
    void game_hooks_remove();
    void player_sync_init();
    void player_sync_shutdown();
    void npc_manager_init();
    void npc_manager_shutdown();
    void ui_init();
    void ui_shutdown();
}

// ---------------------------------------------------------------------------
// Plugin implementation
// ---------------------------------------------------------------------------
class KenshiMPPlugin : public Ogre::Plugin {
public:
    const Ogre::String& getName() const override {
        static Ogre::String name = "KenshiMP";
        return name;
    }

    void install() override {
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin installed");
    }

    void initialise() override {
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Initialising...");

        // Order matters: hooks first, then networking, then sync, then UI
        kmp::game_hooks_install();
        kmp::client_init();
        kmp::npc_manager_init();
        kmp::player_sync_init();
        kmp::ui_init();

        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Initialised OK");
    }

    void shutdown() override {
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Shutting down...");

        kmp::ui_shutdown();
        kmp::player_sync_shutdown();
        kmp::npc_manager_shutdown();
        kmp::client_shutdown();
        kmp::game_hooks_remove();

        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Shut down OK");
    }

    void uninstall() override {
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Plugin uninstalled");
    }
};

// ---------------------------------------------------------------------------
// Singleton + DLL exports
// ---------------------------------------------------------------------------
static KenshiMPPlugin* g_plugin = nullptr;

extern "C" void __declspec(dllexport) dllStartPlugin() {
    g_plugin = new KenshiMPPlugin();
    Ogre::Root::getSingleton().installPlugin(g_plugin);
}

extern "C" void __declspec(dllexport) dllStopPlugin() {
    Ogre::Root::getSingleton().uninstallPlugin(g_plugin);
    delete g_plugin;
    g_plugin = nullptr;
}
