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
#include "kmp_log.h"

#include "packets.h"
#include "protocol.h"
#include "serialization.h"
#include "client_identity.h"

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
extern void player_sync_set_requested_host(bool val);
extern bool host_sync_is_host();

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_ui_initialized = false;
static bool s_ui_visible = false;

// MyGUI widgets
static MyGUI::Window*   s_connect_window = NULL;
static MyGUI::EditBox*  s_host_input     = NULL;
static MyGUI::EditBox*  s_port_input     = NULL;
static MyGUI::Button*   s_host_btn       = NULL;
static MyGUI::Button*   s_join_btn       = NULL;
static MyGUI::Button*   s_disconnect_btn = NULL;
static MyGUI::Button*   s_chat_toggle_btn = NULL;

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
static void on_host_clicked(MyGUI::Widget* sender);
static void on_join_clicked(MyGUI::Widget* sender);
static void on_disconnect_clicked(MyGUI::Widget* sender);
static void on_chat_send_clicked(MyGUI::Widget* sender);
static void on_chat_key_press(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch);
static void on_chat_window_button(MyGUI::Window* sender, const std::string& name);
static void refresh_chat_display();
static void append_system_message(const std::string& text);
static bool handle_chat_command(const std::string& text);
static void update_status_text();

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------
void ui_init() {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) {
        KMP_LOG(
            "[KenshiMP] WARNING: MyGUI not available, UI disabled"
        );
        return;
    }

    try {

    MyGUI::Colour textCol(0.08f, 0.03f, 0.02f);
    std::string stdFont = "Kenshi_StandardFont_Medium";
    std::string btnFont = "Kenshi_PaintedTextFont_Medium";

    // --- Connect dialog ---
    s_connect_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_WindowCX",   // draggable caption bar
        MyGUI::IntCoord(100, 100, 320, 240),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_ConnectWindow"
    );
    s_connect_window->setCaption("KenshiMP - Connect");
    s_connect_window->setVisible(false);

    // Host label + input
    MyGUI::TextBox* host_label = s_connect_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(10, 10, 60, 26),
        MyGUI::Align::Default,
        "KMP_HostLabel"
    );
    host_label->setCaption("Host:");
    host_label->setFontName(stdFont);
    host_label->setTextColour(textCol);

    s_host_input = s_connect_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxEmptySkin",
        MyGUI::IntCoord(75, 10, 220, 26),
        MyGUI::Align::Default,
        "KMP_HostInput"
    );
    s_host_input->setCaption("127.0.0.1");
    s_host_input->setFontName(stdFont);
    s_host_input->setTextColour(textCol);

    // Port label + input
    MyGUI::TextBox* port_label = s_connect_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(10, 44, 60, 26),
        MyGUI::Align::Default,
        "KMP_PortLabel"
    );
    port_label->setCaption("Port:");
    port_label->setFontName(stdFont);
    port_label->setTextColour(textCol);

    s_port_input = s_connect_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxEmptySkin",
        MyGUI::IntCoord(75, 44, 220, 26),
        MyGUI::Align::Default,
        "KMP_PortInput"
    );
    s_port_input->setCaption("7777");
    s_port_input->setFontName(stdFont);
    s_port_input->setTextColour(textCol);

    // Host button
    s_host_btn = s_connect_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(10, 80, 140, 30),
        MyGUI::Align::Default,
        "KMP_HostBtn"
    );
    s_host_btn->setCaption("Host");
    s_host_btn->setFontName(btnFont);
    s_host_btn->setTextAlign(MyGUI::Align::Center);
    s_host_btn->eventMouseButtonClick += MyGUI::newDelegate(on_host_clicked);

    // Join button
    s_join_btn = s_connect_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(160, 80, 140, 30),
        MyGUI::Align::Default,
        "KMP_JoinBtn"
    );
    s_join_btn->setCaption("Join");
    s_join_btn->setFontName(btnFont);
    s_join_btn->setTextAlign(MyGUI::Align::Center);
    s_join_btn->eventMouseButtonClick += MyGUI::newDelegate(on_join_clicked);

    // Disconnect button
    s_disconnect_btn = s_connect_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(10, 120, 290, 30),
        MyGUI::Align::Default,
        "KMP_DisconnectBtn"
    );
    s_disconnect_btn->setCaption("Disconnect");
    s_disconnect_btn->setFontName(btnFont);
    s_disconnect_btn->setTextAlign(MyGUI::Align::Center);
    s_disconnect_btn->eventMouseButtonClick += MyGUI::newDelegate(on_disconnect_clicked);

    // --- Chat window ---
    // Default position is higher on screen (was too low previously) and bigger
    // so several chat lines fit without resizing.
    s_chat_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_WindowCX",   // real window skin with caption bar (so it's draggable)
        MyGUI::IntCoord(40, 120, 460, 360),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_ChatWindow"
    );
    s_chat_window->setCaption("KenshiMP - Chat");
    s_chat_window->setVisible(false);
    // Hook the caption X button. `name` is the button id; "close" is the
    // default for the X button on Kenshi_WindowCX.
    s_chat_window->eventWindowButtonPressed += MyGUI::newDelegate(on_chat_window_button);

    // Fetch the skin-defined client area once; everything below is positioned
    // in client-relative coords already, so MyGUI handles caption offset.
    const int pad   = 6;
    const int input_h = 28;
    const MyGUI::IntSize cs = s_chat_window->getClientCoord().size();

    // Chat display — top area. HStretch + VStretch so it keeps filling the
    // space above the input as the window resizes.
    // Multi-line edit skin (same one Kenshi uses for NPC dialog / description
    // boxes). Kenshi_EditBox by itself is a single-line skin and vertically
    // centres its text, which is what made chat look "floating in the middle".
    s_chat_display = s_chat_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBoxStrechEmpty",
        MyGUI::IntCoord(pad, pad,
                        cs.width  - 2 * pad,
                        cs.height - 3 * pad - input_h),
        MyGUI::Align::Stretch,
        "KMP_ChatDisplay"
    );
    s_chat_display->setEditReadOnly(true);
    s_chat_display->setEditMultiLine(true);
    s_chat_display->setEditWordWrap(true);
    s_chat_display->setEditStatic(true);
    s_chat_display->setFontName(stdFont);
    s_chat_display->setTextColour(textCol);   // dark text on the light editbox bg
    // Anchor text to top-left (default EditBox alignment on some skins centres
    // vertically, which makes the log look like it's floating in the middle).
    s_chat_display->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
    // Show a scrollbar when the log overflows; we scroll to the bottom on new
    // messages in refresh_chat_display().
    s_chat_display->setVisibleVScroll(true);

    // Chat input — full-width, bottom-anchored. Enter submits; no Send button.
    s_chat_input = s_chat_window->createWidget<MyGUI::EditBox>(
        "Kenshi_EditBox",
        MyGUI::IntCoord(pad, cs.height - pad - input_h,
                        cs.width - 2 * pad, input_h),
        MyGUI::Align::HStretch | MyGUI::Align::Bottom,
        "KMP_ChatInput"
    );
    s_chat_input->setFontName(stdFont);
    s_chat_input->setTextColour(textCol);
    s_chat_input->setEditStatic(false);
    s_chat_input->setEditReadOnly(false);
    s_chat_input->setEditMultiLine(false);
    s_chat_input->setNeedKeyFocus(true);
    s_chat_input->eventKeyButtonPressed += MyGUI::newDelegate(on_chat_key_press);
    s_chat_send_btn = NULL;  // legacy, no longer created

    // --- Status text (top of screen) ---
    s_status_text = gui->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(10, 5, 400, 24),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_StatusText"
    );
    s_status_text->setCaption("KenshiMP - F8: connect  F10: chat");
    s_status_text->setFontName(stdFont);
    s_status_text->setTextColour(MyGUI::Colour(0.8f, 1.0f, 0.8f));
    s_status_text->setVisible(true);

    s_ui_initialized = true;
    s_ui_visible = false;

    KMP_LOG("[KenshiMP] UI initialized");

    } catch (std::exception& e) {
        KMP_LOG(
            std::string("[KenshiMP] UI init failed: ") + e.what()
        );
        s_ui_initialized = false;
    } catch (...) {
        KMP_LOG(
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

    // F8 toggles the connect dialog only. The chat window is persistent while
    // connected (shown on accept, hidden on disconnect) so players can always
    // see incoming messages and reach the input field.
    s_ui_visible = !s_ui_visible;
    if (s_connect_window) s_connect_window->setVisible(s_ui_visible);
}

// ---------------------------------------------------------------------------
// Check for F8 key press — call this from player_sync_tick or game hook
// ---------------------------------------------------------------------------
void ui_check_hotkey() {
    // F8: toggle the connect dialog.
    // Note: F8 is also Kenshi's screenshot key — move this to a free key
    // (F6 / F1) if the screenshot conflict bites.
    static bool s_f8_was_down = false;
    bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8_down && !s_f8_was_down) {
        ui_toggle();
    }
    s_f8_was_down = f8_down;

    // F10: toggle chat window. Kenshi doesn't bind F10, so it's safe for us.
    // Use this to reopen the chat after closing it with the X button.
    static bool s_f10_was_down = false;
    bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !s_f10_was_down && s_chat_window && client_is_connected()) {
        bool vis = s_chat_window->getVisible();
        s_chat_window->setVisible(!vis);
        if (!vis && s_chat_input)
            MyGUI::InputManager::getInstance().setKeyFocusWidget(s_chat_input);
    }
    s_f10_was_down = f10_down;
}

// ---------------------------------------------------------------------------
// Callbacks from player_sync
// ---------------------------------------------------------------------------
void ui_on_connect_accept(uint32_t player_id) {
    append_system_message("Connected. Your ID: " + itos(player_id)
        + ".  Type /help for commands.");
    update_status_text();

    // Show chat window whenever connected, regardless of the connect-dialog toggle.
    if (s_chat_window) {
        s_chat_window->setVisible(true);
    }
}

void ui_on_disconnect() {
    append_system_message("Disconnected from server");
    update_status_text();
}

void ui_on_chat(const ChatMessage& pkt) {
    ChatEntry entry;
    if (pkt.player_id == 0) {
        // player_id == 0 is the server sentinel (see session.cpp / admin_broadcast_chat).
        entry.sender = "[Server]";
    } else {
        entry.sender = "Player " + itos(pkt.player_id);
    }
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
// ---------------------------------------------------------------------------
// Embedded server — launch server exe as child process
// ---------------------------------------------------------------------------
static PROCESS_INFORMATION s_server_process;
static bool s_server_running = false;

static void launch_server(uint16_t port) {
    if (s_server_running) return;

    // Find server exe relative to the DLL location
    // Try common paths
    const char* paths[] = {
        "mods\\KenshiMP\\kenshi-mp-server.exe",
        "mods\\KenshiMP\\kenshi-mp-server2.exe",
        "kenshi-mp-server.exe",
    };

    std::string exe_path;
    for (int i = 0; i < 3; ++i) {
        DWORD attr = GetFileAttributesA(paths[i]);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            exe_path = paths[i];
            break;
        }
    }

    if (exe_path.empty()) {
        KMP_LOG(
            "[KenshiMP] Server exe not found! Place kenshi-mp-server.exe in mods/KenshiMP/");
        return;
    }

    STARTUPINFOA si;
    std::memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    std::memset(&s_server_process, 0, sizeof(s_server_process));

    char cmd[512];
    _snprintf(cmd, sizeof(cmd) - 1, "\"%s\"", exe_path.c_str());

    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NEW_CONSOLE, NULL, NULL, &si, &s_server_process)) {
        s_server_running = true;
        KMP_LOG("[KenshiMP] Server launched (PID: " +
            itos(s_server_process.dwProcessId) + ")");
    } else {
        KMP_LOG("[KenshiMP] Failed to launch server");
    }
}

static void stop_server() {
    if (!s_server_running) return;

    TerminateProcess(s_server_process.hProcess, 0);
    CloseHandle(s_server_process.hProcess);
    CloseHandle(s_server_process.hThread);
    s_server_running = false;
    KMP_LOG("[KenshiMP] Server stopped");
}

static void do_connect(bool as_host) {
    KMP_LOG(std::string("[KenshiMP] do_connect: as_host=") + (as_host ? "true (HOST button)" : "false (JOIN button)"));
    if (client_is_connected()) {
        KMP_LOG("[KenshiMP] do_connect: already connected, ignoring click");
        return;
    }
    if (!s_host_input || !s_port_input) return;

    std::string host = s_host_input->getCaption();
    std::string port_str = s_port_input->getCaption();
    uint16_t port = static_cast<uint16_t>(atoi(port_str.c_str()));

    // If hosting, launch the server first
    if (as_host) {
        launch_server(port);
        Sleep(1500);  // give server time to bind ENet socket
    }

    player_sync_set_requested_host(as_host);

    // Retry connect a few times — server may still be binding
    bool connected = false;
    int max_attempts = as_host ? 5 : 1;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (client_connect(host.c_str(), port)) { connected = true; break; }
        KMP_LOG(std::string("[KenshiMP] client_connect attempt ")
            + (char)('0' + attempt) + "/" + (char)('0' + max_attempts) + " failed");
        if (attempt < max_attempts) Sleep(1000);
    }

    if (connected) {
        ConnectRequest req;
        // Build a unique-per-install display name so the server + other clients
        // can tell two joiners apart. "Host" stays explicit; joiners get a 6-hex
        // suffix derived from their stable UUID (first chars of the UUID string,
        // minus the dashes) so the same person always shows up as the same name.
        const char* uuid = client_identity_get_uuid();
        std::string suffix;
        for (const char* c = uuid; *c && suffix.size() < 6; ++c) {
            if (*c != '-') suffix += *c;
        }
        std::string display = as_host ? "Host" : ("Player-" + suffix);
        std::strncpy(req.name, display.c_str(), MAX_NAME_LENGTH - 1);
        req.name[MAX_NAME_LENGTH - 1] = '\0';
        std::strncpy(req.model, "greenlander", MAX_MODEL_LENGTH - 1);
        req.model[MAX_MODEL_LENGTH - 1] = '\0';
        req.is_host = as_host ? 1 : 0;
        std::strncpy(req.client_uuid, client_identity_get_uuid(), sizeof(req.client_uuid) - 1);
        req.client_uuid[sizeof(req.client_uuid) - 1] = '\0';
        std::vector<uint8_t> buf = pack(req);
        client_send_reliable(buf.data(), buf.size());
        KMP_LOG(std::string("[KenshiMP] Sent ConnectRequest name=") + req.name + " is_host=" + (req.is_host ? "1" : "0"));

        update_status_text();
    } else {
        KMP_LOG("[KenshiMP] client_connect FAILED after all attempts");
    }
}

static void on_host_clicked(MyGUI::Widget* sender) {
    do_connect(true);
}

static void on_join_clicked(MyGUI::Widget* sender) {
    do_connect(false);
}

static void on_disconnect_clicked(MyGUI::Widget* sender) {
    if (!client_is_connected() && !s_server_running) return;
    client_disconnect();
    stop_server();
    update_status_text();

    if (s_chat_window) s_chat_window->setVisible(false);
}

static void on_chat_send_clicked(MyGUI::Widget* sender) {
    if (!s_chat_input) return;

    std::string msg = s_chat_input->getCaption();
    if (msg.empty()) return;

    // Intercept / commands locally.
    if (handle_chat_command(msg)) {
        s_chat_input->setCaption("");
        return;
    }

    ui_send_chat(msg.c_str());
    s_chat_input->setCaption("");
}

static void on_chat_key_press(MyGUI::Widget* sender, MyGUI::KeyCode key, MyGUI::Char ch) {
    if (key == MyGUI::KeyCode::Return) {
        on_chat_send_clicked(sender);
    }
}

static void on_chat_window_button(MyGUI::Window* sender, const std::string& name) {
    // The X button on Kenshi_WindowCX sends "close". Hide instead of destroy
    // so the window can be reopened later (F9).
    if (name == "close") {
        if (s_chat_window) s_chat_window->setVisible(false);
    }
}

// Append a "[SYSTEM]" line from our own side (not a network chat packet).
static void append_system_message(const std::string& text) {
    ChatEntry entry;
    entry.sender = "[SYSTEM]";
    entry.message = text;
    s_chat_log.push_back(entry);
    refresh_chat_display();
}

// Return true if the text was a recognized command (and we handled it).
static bool handle_chat_command(const std::string& text) {
    if (text.empty() || text[0] != '/') return false;

    // Split command and arg.
    std::string cmd, arg;
    size_t sp = text.find(' ');
    if (sp == std::string::npos) {
        cmd = text.substr(1);
    } else {
        cmd = text.substr(1, sp - 1);
        arg = text.substr(sp + 1);
    }

    if (cmd == "help") {
        append_system_message("Commands: /help, /clear, /who, /close");
        return true;
    }
    if (cmd == "clear") {
        s_chat_log.clear();
        refresh_chat_display();
        return true;
    }
    if (cmd == "close") {
        if (s_chat_window) s_chat_window->setVisible(false);
        return true;
    }
    if (cmd == "who") {
        append_system_message("Your player id: " + itos(client_get_local_id()));
        return true;
    }

    append_system_message("Unknown command: /" + cmd + "  (try /help)");
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void refresh_chat_display() {
    if (!s_chat_display) return;

    std::string text;
    // Show last 200 messages (bumped from 50 — auto-scroll keeps newest visible).
    size_t start = s_chat_log.size() > 200 ? s_chat_log.size() - 200 : 0;
    for (size_t i = start; i < s_chat_log.size(); ++i) {
        text += s_chat_log[i].sender + ": " + s_chat_log[i].message + "\n";
    }

    s_chat_display->setCaption(text);
    // Scroll to bottom so the newest message is always visible. Moving the
    // cursor past the last char forces MyGUI's EditBox to scroll its view.
    s_chat_display->setTextCursor(text.size());
    size_t vrange = s_chat_display->getVScrollRange();
    if (vrange > 0) s_chat_display->setVScrollPosition(vrange - 1);
}

static void update_status_text() {
    if (!s_status_text) return;

    if (client_is_connected()) {
        std::string role = host_sync_is_host() ? "HOSTING" : "JOINED";
        s_status_text->setCaption(
            "KenshiMP - " + role + " as Player #" + itos(client_get_local_id())
        );
        s_status_text->setTextColour(MyGUI::Colour(0.4f, 1.0f, 0.4f));
    } else {
        s_status_text->setCaption("KenshiMP - Disconnected  (F8: connect)");
        s_status_text->setTextColour(MyGUI::Colour(1.0f, 0.6f, 0.6f));
    }
}

} // namespace kmp
