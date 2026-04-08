// admin_panel.cpp — Host admin/debug panel (F10)
//
// MyGUI window showing connected players, synced NPC stats,
// and controls for spawning test NPCs.

#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MyGUI.h>
#include <OgreLogManager.h>

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/Faction.h>
#include <kenshi/util/hand.h>
#include <OgreVector3.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

extern bool client_is_connected();
extern uint32_t client_get_local_id();
extern bool host_sync_is_host();
extern uint32_t host_sync_get_synced_count();
extern void host_sync_spawn_test_npc(float x, float y, float z);
extern Character* game_get_player_character();
extern RootObjectFactory* game_get_factory();
extern GameWorld* game_get_world();

// v100-safe int to string
static std::string itos(uint32_t val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

static std::string ftos(float val, int precision) {
    std::ostringstream ss;
    ss.precision(precision);
    ss << std::fixed << val;
    return ss.str();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_initialized = false;
static bool s_visible = false;
static float s_update_timer = 0.0f;

// Widgets
static MyGUI::Window*   s_window = NULL;
static MyGUI::EditBox*  s_player_list = NULL;
static MyGUI::TextBox*  s_npc_stats = NULL;
static MyGUI::Button*   s_spawn_btn = NULL;
static MyGUI::Button*   s_clear_btn = NULL;
static MyGUI::Button*   s_give_katana_btn = NULL;
static MyGUI::Button*   s_give_plank_btn = NULL;
static MyGUI::Button*   s_give_crossbow_btn = NULL;
static MyGUI::Button*   s_give_armour_btn = NULL;
static MyGUI::Button*   s_give_medkit_btn = NULL;
static MyGUI::Button*   s_give_food_btn = NULL;
static MyGUI::TextBox*  s_items_label = NULL;

// Track remote player positions (from received PLAYER_STATE packets)
struct RemotePlayerInfo {
    uint32_t id;
    float x, y, z;
};
static std::map<uint32_t, RemotePlayerInfo> s_known_players;

// ---------------------------------------------------------------------------
// Give item helper
// ---------------------------------------------------------------------------
static void give_item_to_player(const char* item_id) {
    if (!ou) return;
    Character* ch = game_get_player_character();
    if (!ch) return;

    RootObjectFactory* factory = ou->theFactory;
    if (!factory) return;

    // Search by GameData::stringID (human-readable name)
    // The gamedataSID map is keyed by composite IDs, not human names
    GameDataManager& gdm = ou->gamedata;
    GameData* gd = NULL;
    std::string search(item_id);

    // Search by GameData::name (display name at offset 0x28)
    ogre_unordered_map<std::string, GameData*>::type::iterator it;
    for (it = gdm.gamedataSID.begin(); it != gdm.gamedataSID.end(); ++it) {
        if (!it->second) continue;
        if (it->second->name == search) {
            gd = it->second;
            break;
        }
    }

    if (!gd) {
        Ogre::LogManager::getSingleton().logMessage(
            std::string("[KenshiMP] Item not found by name: ") + item_id);

        // Log items whose name contains our search (case-insensitive)
        int count = 0;
        for (it = gdm.gamedataSID.begin(); it != gdm.gamedataSID.end() && count < 15; ++it) {
            if (!it->second) continue;
            std::string n = it->second->name;
            bool found = false;
            for (size_t i = 0; i + search.size() <= n.size() && !found; ++i) {
                bool ok = true;
                for (size_t j = 0; j < search.size() && ok; ++j) {
                    char a = n[i+j]; if (a >= 'A' && a <= 'Z') a += 32;
                    char b = search[j]; if (b >= 'A' && b <= 'Z') b += 32;
                    if (a != b) ok = false;
                }
                if (ok) found = true;
            }
            if (found) {
                Ogre::LogManager::getSingleton().logMessage(
                    std::string("[KenshiMP]   match: '") + n + "'");
                count++;
            }
        }
        return;
    }

    // Use the full createItem overload
    hand h;
    Item* item = factory->createItem(gd, h, NULL, NULL, -1, NULL);
    if (item) {
        ch->giveItem(item, true, false);
        Ogre::LogManager::getSingleton().logMessage(
            std::string("[KenshiMP] Gave item: ") + item_id);
    } else {
        Ogre::LogManager::getSingleton().logMessage(
            std::string("[KenshiMP] Failed to create item: ") + item_id);
    }
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void on_spawn_clicked(MyGUI::Widget* sender);
static void on_clear_clicked(MyGUI::Widget* sender);
static void on_give_katana(MyGUI::Widget* sender);
static void on_give_plank(MyGUI::Widget* sender);
static void on_give_crossbow(MyGUI::Widget* sender);
static void on_give_armour(MyGUI::Widget* sender);
static void on_give_medkit(MyGUI::Widget* sender);
static void on_give_food(MyGUI::Widget* sender);

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void admin_panel_init() {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;

    try {

    s_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_GenericWindowSkin",
        MyGUI::IntCoord(50, 50, 500, 550),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_AdminWindow"
    );
    s_window->setCaption("KenshiMP Admin Panel");
    s_window->setVisible(false);

    MyGUI::Colour textCol(0.08f, 0.03f, 0.02f);
    std::string font = "Kenshi_StandardFont_Medium";
    std::string btnFont = "Kenshi_PaintedTextFont_Medium";

    // --- Player list header ---
    MyGUI::TextBox* player_label = s_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(10, 5, 200, 22),
        MyGUI::Align::Default,
        "KMP_AdminPlayerLabel"
    );
    player_label->setCaption("Connected Players:");
    player_label->setFontName(font);
    player_label->setTextColour(textCol);

    // Player list (read-only multiline)
    s_player_list = s_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxEmptySkin",
        MyGUI::IntCoord(10, 28, 470, 150),
        MyGUI::Align::Default,
        "KMP_AdminPlayerList"
    );
    s_player_list->setEditReadOnly(true);
    s_player_list->setEditMultiLine(true);
    s_player_list->setFontName(font);
    s_player_list->setTextColour(textCol);

    // --- NPC stats ---
    s_npc_stats = s_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(10, 185, 470, 50),
        MyGUI::Align::Default,
        "KMP_AdminNPCStats"
    );
    s_npc_stats->setCaption("Synced NPCs: 0");
    s_npc_stats->setFontName(font);
    s_npc_stats->setTextColour(textCol);

    // --- Controls ---
    MyGUI::TextBox* ctrl_label = s_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(10, 245, 200, 22),
        MyGUI::Align::Default,
        "KMP_AdminCtrlLabel"
    );
    ctrl_label->setCaption("Controls:");
    ctrl_label->setFontName(font);
    ctrl_label->setTextColour(textCol);

    s_spawn_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(10, 270, 220, 35),
        MyGUI::Align::Default,
        "KMP_AdminSpawnBtn"
    );
    s_spawn_btn->setCaption("Spawn Test NPC Here");
    s_spawn_btn->setFontName("Kenshi_PaintedTextFont_Medium");
    s_spawn_btn->setTextAlign(MyGUI::Align::Center);
    s_spawn_btn->eventMouseButtonClick += MyGUI::newDelegate(on_spawn_clicked);

    s_clear_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(250, 270, 220, 35),
        MyGUI::Align::Default,
        "KMP_AdminClearBtn"
    );
    s_clear_btn->setCaption("Clear All Remote NPCs");
    s_clear_btn->setFontName("Kenshi_PaintedTextFont_Medium");
    s_clear_btn->setTextAlign(MyGUI::Align::Center);
    s_clear_btn->eventMouseButtonClick += MyGUI::newDelegate(on_clear_clicked);

    // --- Give Items ---
    s_items_label = s_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(10, 315, 200, 22),
        MyGUI::Align::Default,
        "KMP_AdminItemsLabel"
    );
    s_items_label->setCaption("Give Items:");
    s_items_label->setFontName(font);
    s_items_label->setTextColour(textCol);

    // Row 1: Katana, Plank, Crossbow
    s_give_katana_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(10, 340, 150, 30),
        MyGUI::Align::Default,
        "KMP_GiveKatana"
    );
    s_give_katana_btn->setCaption("Katana");
    s_give_katana_btn->setFontName(btnFont);
    s_give_katana_btn->setTextAlign(MyGUI::Align::Center);
    s_give_katana_btn->eventMouseButtonClick += MyGUI::newDelegate(on_give_katana);

    s_give_plank_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(170, 340, 150, 30),
        MyGUI::Align::Default,
        "KMP_GivePlank"
    );
    s_give_plank_btn->setCaption("Plank");
    s_give_plank_btn->setFontName(btnFont);
    s_give_plank_btn->setTextAlign(MyGUI::Align::Center);
    s_give_plank_btn->eventMouseButtonClick += MyGUI::newDelegate(on_give_plank);

    s_give_crossbow_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(330, 340, 150, 30),
        MyGUI::Align::Default,
        "KMP_GiveCrossbow"
    );
    s_give_crossbow_btn->setCaption("Crossbow");
    s_give_crossbow_btn->setFontName(btnFont);
    s_give_crossbow_btn->setTextAlign(MyGUI::Align::Center);
    s_give_crossbow_btn->eventMouseButtonClick += MyGUI::newDelegate(on_give_crossbow);

    // Row 2: Armour, Medkit, Food
    s_give_armour_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(10, 375, 150, 30),
        MyGUI::Align::Default,
        "KMP_GiveArmour"
    );
    s_give_armour_btn->setCaption("Armour");
    s_give_armour_btn->setFontName(btnFont);
    s_give_armour_btn->setTextAlign(MyGUI::Align::Center);
    s_give_armour_btn->eventMouseButtonClick += MyGUI::newDelegate(on_give_armour);

    s_give_medkit_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(170, 375, 150, 30),
        MyGUI::Align::Default,
        "KMP_GiveMedkit"
    );
    s_give_medkit_btn->setCaption("Medkit");
    s_give_medkit_btn->setFontName(btnFont);
    s_give_medkit_btn->setTextAlign(MyGUI::Align::Center);
    s_give_medkit_btn->eventMouseButtonClick += MyGUI::newDelegate(on_give_medkit);

    s_give_food_btn = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(330, 375, 150, 30),
        MyGUI::Align::Default,
        "KMP_GiveFood"
    );
    s_give_food_btn->setCaption("Food");
    s_give_food_btn->setFontName(btnFont);
    s_give_food_btn->setTextAlign(MyGUI::Align::Center);
    s_give_food_btn->eventMouseButtonClick += MyGUI::newDelegate(on_give_food);

    s_initialized = true;
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Admin panel initialized");

    } catch (...) {
        Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Admin panel init failed");
        s_initialized = false;
    }
}

void admin_panel_shutdown() {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (gui && s_window) {
        gui->destroyWidget(s_window);
        s_window = NULL;
    }
    s_initialized = false;
    s_known_players.clear();
}

// ---------------------------------------------------------------------------
// Toggle (F10)
// ---------------------------------------------------------------------------
void admin_panel_toggle() {
    if (!s_initialized) return;
    if (!host_sync_is_host()) return;

    s_visible = !s_visible;
    if (s_window) s_window->setVisible(s_visible);
}

void admin_panel_check_hotkey() {
    static bool s_f10_was_down = false;
    bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !s_f10_was_down) {
        admin_panel_toggle();
    }
    s_f10_was_down = f10_down;
}

// ---------------------------------------------------------------------------
// Track remote players (called from packet dispatch)
// ---------------------------------------------------------------------------
void admin_panel_on_player_state(uint32_t player_id, float x, float y, float z) {
    RemotePlayerInfo info;
    info.id = player_id;
    info.x = x;
    info.y = y;
    info.z = z;
    s_known_players[player_id] = info;
}

void admin_panel_on_player_disconnect(uint32_t player_id) {
    s_known_players.erase(player_id);
}

// ---------------------------------------------------------------------------
// Update display (called every frame, but only refreshes every 0.5s)
// ---------------------------------------------------------------------------
void admin_panel_update(float dt) {
    if (!s_initialized || !s_visible) return;

    s_update_timer += dt;
    if (s_update_timer < 0.5f) return;
    s_update_timer = 0.0f;

    // --- Update player list ---
    if (s_player_list) {
        std::string text;

        // Local player (host)
        Character* local_ch = game_get_player_character();
        if (local_ch) {
            Ogre::Vector3 pos = local_ch->getPosition();
            text += "ID " + itos(client_get_local_id()) + " | Player (HOST) | "
                + ftos(pos.x, 0) + ", " + ftos(pos.y, 0) + ", " + ftos(pos.z, 0) + "\n";
        }

        // Remote players
        std::map<uint32_t, RemotePlayerInfo>::iterator it;
        for (it = s_known_players.begin(); it != s_known_players.end(); ++it) {
            text += "ID " + itos(it->second.id) + " | Player | "
                + ftos(it->second.x, 0) + ", " + ftos(it->second.y, 0) + ", "
                + ftos(it->second.z, 0) + "\n";
        }

        if (text.empty()) {
            text = "(no players connected)";
        }

        s_player_list->setCaption(text);
    }

    // --- Update NPC stats ---
    if (s_npc_stats) {
        uint32_t npc_count = host_sync_get_synced_count();
        std::string stats = "Synced NPCs: " + itos(npc_count);
        s_npc_stats->setCaption(stats);
    }
}

// ---------------------------------------------------------------------------
// Button callbacks
// ---------------------------------------------------------------------------
static void on_spawn_clicked(MyGUI::Widget* sender) {
    Character* ch = game_get_player_character();
    if (!ch) return;

    Ogre::Vector3 pos = ch->getPosition();
    host_sync_spawn_test_npc(pos.x, pos.y, pos.z);
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Admin: Spawned test NPC at player position");
}

static void on_clear_clicked(MyGUI::Widget* sender) {
    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] Admin: Clear remote NPCs (not yet implemented)");
}

static void on_give_katana(MyGUI::Widget* sender) { give_item_to_player("Katana"); }
static void on_give_plank(MyGUI::Widget* sender) { give_item_to_player("Plank"); }
static void on_give_crossbow(MyGUI::Widget* sender) { give_item_to_player("Toothpick"); }
static void on_give_armour(MyGUI::Widget* sender) { give_item_to_player("Armoured Rags"); }
static void on_give_medkit(MyGUI::Widget* sender) { give_item_to_player("Standard first aid kit"); }
static void on_give_food(MyGUI::Widget* sender) { give_item_to_player("Dried Meat"); }

} // namespace kmp
