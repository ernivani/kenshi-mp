// ui.cpp — MyGUI-based UI overlay for KenshiMP
//
// Kenshi uses MyGUI for its in-game UI. We access the same MyGUI instance
// to add our own windows: connect dialog, chat, and status bar.
//
// All MyGUI calls happen on the main/render thread (called from player_sync_tick
// via the game loop hook), so they are thread-safe.

#include <string>
#include <sstream>
#include <vector>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MyGUI.h>
#include <OgreLogManager.h>

#include "packets.h"
#include "protocol.h"
#include "serialization.h"

namespace kmp {

static std::string itos(uint32_t val) {
    std::ostringstream ss;
    ss << val;
    return ss.str();
}

// External
extern bool client_connect(const char* host, uint16_t port);
extern void client_disconnect();
extern bool client_is_connected();
extern uint32_t client_get_local_id();
extern void client_send_reliable(const uint8_t* data, size_t length);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_ui_initialized = false;
static bool s_ui_visible = false;

// MyGUI widgets
static MyGUI::Window*   s_connect_window = NULL;
static MyGUI::EditBox*  s_host_input     = NULL;
static MyGUI::EditBox*  s_port_input     = NULL;
static MyGUI::Button*   s_connect_btn    = NULL;
static MyGUI::Button*   s_disconnect_btn = NULL;

static MyGUI::Window*   s_chat_window    = NULL;
static MyGUI::EditBox*  s_chat_display   = NULL;
static MyGUI::EditBox*  s_chat_input     = NULL;
static MyGUI::Button*   s_chat_send_btn  = NULL;

static MyGUI::TextBox*  s_status_text    = NULL;

// Chat log
struct ChatEntry {
    std::string sender;
    std::string message;
};
static std::vector<ChatEntry> s_chat_log;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void on_connect_clicked(MyGUI::Widget* sender);
static void on_disconnect_clicked(MyGUI::Widget* sender);
static void on_chat_send_clicked(MyGUI::Widget* sender);
static void on_chat_key_press(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);
static void refresh_chat_display();
static void update_status_text();

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void ui_init() {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] WARNING: MyGUI not available, UI disabled"
        );
        return;
    }

    try {

    // --- Connect dialog ---
    s_connect_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_GenericWindowSkin",
        MyGUI::IntCoord(100, 100, 320, 200),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_ConnectWindow"
    );
    s_connect_window->setCaption("KenshiMP - Connect");
    s_connect_window->setVisible(false);

    // Host label + input
    MyGUI::TextBox* host_label = s_connect_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextboxStandardText",
        MyGUI::IntCoord(10, 10, 60, 26),
        MyGUI::Align::Default,
        "KMP_HostLabel"
    );
    host_label->setCaption("Host:");

    s_host_input = s_connect_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxStandardText",
        MyGUI::IntCoord(75, 10, 220, 26),
        MyGUI::Align::Default,
        "KMP_HostInput"
    );
    s_host_input->setCaption("127.0.0.1");

    // Port label + input
    MyGUI::TextBox* port_label = s_connect_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextboxStandardText",
        MyGUI::IntCoord(10, 44, 60, 26),
        MyGUI::Align::Default,
        "KMP_PortLabel"
    );
    port_label->setCaption("Port:");

    s_port_input = s_connect_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxStandardText",
        MyGUI::IntCoord(75, 44, 220, 26),
        MyGUI::Align::Default,
        "KMP_PortInput"
    );
    s_port_input->setCaption("7777");

    // Connect button
    s_connect_btn = s_connect_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(10, 80, 140, 30),
        MyGUI::Align::Default,
        "KMP_ConnectBtn"
    );
    s_connect_btn->setCaption("Connect");
    s_connect_btn->eventMouseButtonClick += MyGUI::newDelegate(on_connect_clicked);

    // Disconnect button
    s_disconnect_btn = s_connect_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(160, 80, 140, 30),
        MyGUI::Align::Default,
        "KMP_DisconnectBtn"
    );
    s_disconnect_btn->setCaption("Disconnect");
    s_disconnect_btn->eventMouseButtonClick += MyGUI::newDelegate(on_disconnect_clicked);

    // --- Chat window ---
    s_chat_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_GenericWindowSkin",
        MyGUI::IntCoord(100, 320, 400, 250),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_ChatWindow"
    );
    s_chat_window->setCaption("KenshiMP - Chat");
    s_chat_window->setVisible(false);

    // Chat display (read-only, multiline)
    s_chat_display = s_chat_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxStandardText",
        MyGUI::IntCoord(5, 5, 380, 170),
        MyGUI::Align::Stretch,
        "KMP_ChatDisplay"
    );
    s_chat_display->setEditReadOnly(true);
    s_chat_display->setEditMultiLine(true);
    s_chat_display->setEditWordWrap(true);

    // Chat input
    s_chat_input = s_chat_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxStandardText",
        MyGUI::IntCoord(5, 180, 310, 26),
        MyGUI::Align::Default,
        "KMP_ChatInput"
    );
    s_chat_input->eventKeyButtonPressed += MyGUI::newDelegate(on_chat_key_press);

    // Send button
    s_chat_send_btn = s_chat_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(320, 180, 70, 26),
        MyGUI::Align::Default,
        "KMP_ChatSendBtn"
    );
    s_chat_send_btn->setCaption("Send");
    s_chat_send_btn->eventMouseButtonClick += MyGUI::newDelegate(on_chat_send_clicked);

    // --- Status text (top of screen) ---
    s_status_text = gui->createWidget<MyGUI::TextBox>(
        "Kenshi_TextboxStandardText",
        MyGUI::IntCoord(10, 5, 400, 24),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_StatusText"
    );
    s_status_text->setCaption("KenshiMP - Press F8 to open");
    s_status_text->setTextColour(MyGUI::Colour(0.8f, 1.0f, 0.8f));
    s_status_text->setVisible(true);

    s_ui_initialized = true;
    s_ui_visible = false;

    Ogre::LogManager::getSingleton().logMessage("[KenshiMP] UI initialized");

    } catch (std::exception& e) {
        Ogre::LogManager::getSingleton().logMessage(
            std::string("[KenshiMP] UI init failed: ") + e.what()
        );
        s_ui_initialized = false;
    } catch (...) {
        Ogre::LogManager::getSingleton().logMessage(
            "[KenshiMP] UI init failed: unknown exception"
        );
        s_ui_initialized = false;
    }
}

void ui_shutdown() {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (gui) {
        if (s_connect_window) { gui->destroyWidget(s_connect_window); s_connect_window = NULL; }
        if (s_chat_window)    { gui->destroyWidget(s_chat_window);    s_chat_window = NULL; }
        if (s_status_text)    { gui->destroyWidget(s_status_text);    s_status_text = NULL; }
    }

    s_chat_log.clear();
    s_ui_initialized = false;
}

// ---------------------------------------------------------------------------
// Toggle visibility (called on F8 press)
// ---------------------------------------------------------------------------
void ui_toggle() {
    if (!s_ui_initialized) return;

    s_ui_visible = !s_ui_visible;
    if (s_connect_window) s_connect_window->setVisible(s_ui_visible);
    if (s_chat_window && client_is_connected()) s_chat_window->setVisible(s_ui_visible);
}

// ---------------------------------------------------------------------------
// Check for F8 key press — call this from player_sync_tick or game hook
// ---------------------------------------------------------------------------
void ui_check_hotkey() {
    // F8: toggle UI visibility
    static bool s_f8_was_down = false;
    bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8_down && !s_f8_was_down) {
        ui_toggle();
    }
    s_f8_was_down = f8_down;

    // F9: auto-connect to localhost:7777 (no GUI needed)
    static bool s_f9_was_down = false;
    bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9_down && !s_f9_was_down) {
        if (!client_is_connected()) {
            Ogre::LogManager::getSingleton().logMessage("[KenshiMP] F9: Connecting to 127.0.0.1:7777...");
            if (client_connect("127.0.0.1", 7777)) {
                ConnectRequest req;
                std::strncpy(req.name, "Player", MAX_NAME_LENGTH - 1);
                req.name[MAX_NAME_LENGTH - 1] = '\0';
                std::strncpy(req.model, "greenlander", MAX_MODEL_LENGTH - 1);
                req.model[MAX_MODEL_LENGTH - 1] = '\0';
                std::vector<uint8_t> buf = pack(req);
                client_send_reliable(buf.data(), buf.size());
                Ogre::LogManager::getSingleton().logMessage("[KenshiMP] F9: Connected!");
            } else {
                Ogre::LogManager::getSingleton().logMessage("[KenshiMP] F9: Connection failed!");
            }
        } else {
            Ogre::LogManager::getSingleton().logMessage("[KenshiMP] F9: Disconnecting...");
            client_disconnect();
        }
    }
    s_f9_was_down = f9_down;
}

// ---------------------------------------------------------------------------
// Callbacks from player_sync
// ---------------------------------------------------------------------------
void ui_on_connect_accept(uint32_t player_id) {
    ChatEntry entry;
    entry.sender = "[KenshiMP]";
    entry.message = "Connected! Your ID: " + itos(player_id);
    s_chat_log.push_back(entry);
    refresh_chat_display();
    update_status_text();

    // Show chat window
    if (s_chat_window && s_ui_visible) {
        s_chat_window->setVisible(true);
    }
}

void ui_on_disconnect() {
    ChatEntry entry;
    entry.sender = "[KenshiMP]";
    entry.message = "Disconnected from server";
    s_chat_log.push_back(entry);
    refresh_chat_display();
    update_status_text();
}

void ui_on_chat(const ChatMessage& pkt) {
    ChatEntry entry;
    entry.sender = "Player " + itos(pkt.player_id);
    entry.message = pkt.message;
    s_chat_log.push_back(entry);
    refresh_chat_display();
}

// ---------------------------------------------------------------------------
// UI actions
// ---------------------------------------------------------------------------
void ui_send_chat(const char* message) {
    if (!client_is_connected()) return;
    if (!message || message[0] == '\0') return;

    ChatMessage pkt;
    pkt.player_id = client_get_local_id();
    std::strncpy(pkt.message, message, MAX_CHAT_LENGTH - 1);
    pkt.message[MAX_CHAT_LENGTH - 1] = '\0';

    std::vector<uint8_t> buf = pack(pkt);
    client_send_reliable(buf.data(), buf.size());
}

// ---------------------------------------------------------------------------
// Widget callbacks
// ---------------------------------------------------------------------------
static void on_connect_clicked(MyGUI::Widget* sender) {
    if (client_is_connected()) return;
    if (!s_host_input || !s_port_input) return;

    std::string host = s_host_input->getCaption();
    std::string port_str = s_port_input->getCaption();
    uint16_t port = static_cast<uint16_t>(std::stoi(port_str));

    // Send connect request after ENet connection
    if (client_connect(host.c_str(), port)) {
        ConnectRequest req;
        std::strncpy(req.name, "Player", MAX_NAME_LENGTH - 1);
        req.name[MAX_NAME_LENGTH - 1] = '\0';
        std::strncpy(req.model, "greenlander", MAX_MODEL_LENGTH - 1);
        req.model[MAX_MODEL_LENGTH - 1] = '\0';
        std::vector<uint8_t> buf = pack(req);
        client_send_reliable(buf.data(), buf.size());

        update_status_text();
    }
}

static void on_disconnect_clicked(MyGUI::Widget* sender) {
    if (!client_is_connected()) return;
    client_disconnect();
    update_status_text();

    if (s_chat_window) s_chat_window->setVisible(false);
}

static void on_chat_send_clicked(MyGUI::Widget* sender) {
    if (!s_chat_input) return;

    std::string msg = s_chat_input->getCaption();
    if (msg.empty()) return;

    ui_send_chat(msg.c_str());
    s_chat_input->setCaption("");
}

static void on_chat_key_press(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch) {
    if (key == MyGUI::KeyCode::Return) {
        on_chat_send_clicked(sender);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void refresh_chat_display() {
    if (!s_chat_display) return;

    std::string text;
    // Show last 50 messages
    size_t start = s_chat_log.size() > 50 ? s_chat_log.size() - 50 : 0;
    for (size_t i = start; i < s_chat_log.size(); ++i) {
        text += s_chat_log[i].sender + ": " + s_chat_log[i].message + "\n";
    }

    s_chat_display->setCaption(text);
}

static void update_status_text() {
    if (!s_status_text) return;

    if (client_is_connected()) {
        s_status_text->setCaption(
            "KenshiMP — Connected as Player #" + itos(client_get_local_id())
        );
        s_status_text->setTextColour(MyGUI::Colour(0.4f, 1.0f, 0.4f));
    } else {
        s_status_text->setCaption("KenshiMP — Disconnected (F8 to open)");
        s_status_text->setTextColour(MyGUI::Colour(1.0f, 0.6f, 0.6f));
    }
}

} // namespace kmp
