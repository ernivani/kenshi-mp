// ui.cpp — MyGUI-based UI overlay for KenshiMP
//
// Kenshi uses MyGUI for its in-game UI. Since we're loaded as a plugin,
// we can access the same MyGUI instance to add our own windows.
//
// Provides:
//   - Server connect dialog (IP:port input)
//   - Connected players list
//   - Chat window

#include <string>
#include <vector>
#include <cstring>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

// MyGUI headers (when available)
// #include <MyGUI.h>
// #include <MyGUI_OgrePlatform.h>

namespace kmp {

// External
extern bool client_connect(const char* host, uint16_t port);
extern void client_disconnect();
extern bool client_is_connected();
extern uint32_t client_get_local_id();
extern void client_send_reliable(const uint8_t* data, size_t length);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_ui_visible = false;

// Chat log
struct ChatEntry {
    std::string sender;
    std::string message;
};
static std::vector<ChatEntry> s_chat_log;

// Connected players (for display)
struct PlayerInfo {
    uint32_t    id;
    std::string name;
};
static std::vector<PlayerInfo> s_player_list;

// MyGUI widget pointers (set during init)
// static MyGUI::Window*   s_connect_window = nullptr;
// static MyGUI::EditBox*  s_host_input     = nullptr;
// static MyGUI::EditBox*  s_port_input     = nullptr;
// static MyGUI::Window*   s_chat_window    = nullptr;
// static MyGUI::EditBox*  s_chat_input     = nullptr;
// static MyGUI::EditBox*  s_chat_display   = nullptr;
// static MyGUI::ListBox*  s_player_listbox = nullptr;

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void ui_init() {
    // TODO: Create MyGUI windows
    //
    // MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    // if (!gui) return;
    //
    // -- Connect dialog --
    // s_connect_window = gui->createWidget<MyGUI::Window>(
    //     "WindowCSX", MyGUI::IntCoord(100, 100, 300, 180),
    //     MyGUI::Align::Default, "Overlapped", "KMP_ConnectWindow"
    // );
    // s_connect_window->setCaption("KenshiMP - Connect");
    // s_connect_window->setVisible(false);
    //
    // s_host_input = s_connect_window->createWidget<MyGUI::EditBox>(...);
    // s_port_input = s_connect_window->createWidget<MyGUI::EditBox>(...);
    // auto* connect_btn = s_connect_window->createWidget<MyGUI::Button>(...);
    // connect_btn->eventMouseButtonClick += MyGUI::newDelegate(on_connect_clicked);
    //
    // -- Chat window --
    // Similar setup for chat display + input

    s_ui_visible = false;
}

void ui_shutdown() {
    // TODO: Destroy MyGUI widgets
    // MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    // if (gui && s_connect_window) gui->destroyWidget(s_connect_window);
    // if (gui && s_chat_window)    gui->destroyWidget(s_chat_window);

    s_chat_log.clear();
    s_player_list.clear();
}

// ---------------------------------------------------------------------------
// Toggle visibility (bound to a key, e.g., F8)
// ---------------------------------------------------------------------------
void ui_toggle() {
    s_ui_visible = !s_ui_visible;
    // if (s_connect_window) s_connect_window->setVisible(s_ui_visible);
    // if (s_chat_window)    s_chat_window->setVisible(s_ui_visible);
}

// ---------------------------------------------------------------------------
// Callbacks from player_sync
// ---------------------------------------------------------------------------
void ui_on_connect_accept(uint32_t player_id) {
    ChatEntry entry;
    entry.sender = "[KenshiMP]";
    entry.message = "Connected! Your ID: " + std::to_string(player_id);
    s_chat_log.push_back(entry);

    // TODO: update chat display widget
    // TODO: hide connect dialog, show chat
}

void ui_on_chat(const ChatMessage& pkt) {
    ChatEntry entry;
    entry.sender = "Player " + std::to_string(pkt.player_id);
    entry.message = pkt.message;
    s_chat_log.push_back(entry);

    // TODO: append to chat display widget
}

// ---------------------------------------------------------------------------
// UI actions
// ---------------------------------------------------------------------------
void ui_send_chat(const char* message) {
    if (!client_is_connected()) return;

    ChatMessage pkt;
    pkt.player_id = client_get_local_id();
    safe_strcpy(pkt.message, message);

    auto buf = pack(pkt);
    client_send_reliable(buf.data(), buf.size());

    // Add to local chat log
    ChatEntry entry;
    entry.sender = "You";
    entry.message = message;
    s_chat_log.push_back(entry);
}

// Called when user clicks "Connect" button
static void on_connect_clicked(const char* host, uint16_t port) {
    if (client_is_connected()) {
        client_disconnect();
    }
    client_connect(host, port);
}

} // namespace kmp
