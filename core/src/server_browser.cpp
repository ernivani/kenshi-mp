// server_browser.cpp — see server_browser.h
#include "server_browser.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MyGUI.h>
#include <OgreLogManager.h>
#include <kenshi/gui/GUIWindow.h>

#include <enet/enet.h>

// Forward-decl TitleScreen (see ui.cpp for the reason — TitleScreen.h
// redefines GUIWindow inline and collides with GUIWindow.h).
class TitleScreen : public GUIWindow {
public:
    static TitleScreen* getSingleton();
};

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kmp_log.h"
#include "packets.h"
#include "serialization.h"
#include "server_list.h"
#include "server_pinger.h"

namespace kmp {

namespace {

struct RowWidgets {
    MyGUI::Button*  root;
    MyGUI::TextBox* line1;
    MyGUI::TextBox* line2;
};

static bool                          s_open = false;
static MyGUI::Widget*                s_backdrop = NULL;
static MyGUI::Window*                s_window = NULL;
static MyGUI::Button*                s_btn_refresh = NULL;
static MyGUI::Button*                s_btn_back    = NULL;
static MyGUI::Button*                s_btn_direct  = NULL;
static MyGUI::Button*                s_btn_add     = NULL;
static MyGUI::Button*                s_btn_edit    = NULL;
static MyGUI::Button*                s_btn_remove  = NULL;
static MyGUI::Button*                s_btn_join    = NULL;
static MyGUI::Widget*                s_list_scroll = NULL;  // container for rows (top-anchored)
static std::vector<ServerEntry>      s_entries;
static std::map<std::string, RowWidgets> s_rows;
static std::string                   s_selected_id;

// Ping transport (separate ENet host; does NOT interfere with the main game's client).
static ENetHost*                     s_ping_host = NULL;
static std::unique_ptr<ServerPinger> s_pinger;
static std::map<ENetPeer*, std::string> s_peer_to_id;
static std::map<std::string, ENetPeer*> s_id_to_peer;

// Add/Edit dialog
static MyGUI::Window*   s_add_window = NULL;
static MyGUI::EditBox*  s_add_name   = NULL;
static MyGUI::EditBox*  s_add_addr   = NULL;  // "host" or "host:port"
static MyGUI::EditBox*  s_add_pw     = NULL;
static MyGUI::TextBox*  s_add_err    = NULL;
static MyGUI::Button*   s_add_ok     = NULL;
static MyGUI::Button*   s_add_cancel = NULL;
static bool             s_add_is_edit   = false;  // editing existing entry
static bool             s_add_is_direct = false;  // direct connect (no save)

static void rebuild_rows();
static void update_button_states();
static void start_all_pings();
static void stop_all_pings();
static void apply_ping_to_row(const std::string& id);
static void on_add_ok(MyGUI::Widget*);
static void on_add_cancel(MyGUI::Widget*);

// Hide the caption bar + close button + minimise/maximise on a MyGUI
// Window. Kenshi_WindowCX ships with these as child widgets named
// "Caption", "Button_Close", etc. For our modals we want a frameless
// panel look — no drag bar, no X. Any child we don't find is silently
// skipped.
static void hide_window_chrome(MyGUI::Widget* w) {
    if (!w) return;
    const char* names[] = {
        "Caption", "Button_Close", "Button_Minimize", "Button_Maximize",
        "Close", "Minimize", "Maximize",
        NULL
    };
    for (const char** n = names; *n; ++n) {
        MyGUI::Widget* child = w->findWidget(*n);
        if (child) child->setVisible(false);
    }
}

// Try creating a Window with a skin that has no caption bar / close
// button. Kenshi's Options window uses a "chromeless" variant — we
// probe common names and fall back to Kenshi_WindowCX + hide_window_
// chrome if nothing else works.
static MyGUI::Window* create_chromeless_window(
        MyGUI::Widget* parent_or_gui_root_signal,  // NULL → use Gui root
        const MyGUI::IntCoord& coord,
        MyGUI::Align align,
        const char* layer,   // used only if parent is NULL
        const char* name) {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return NULL;

    const char* skins[] = {
        "Kenshi_Window",
        "Kenshi_WindowC",
        "Kenshi_WindowCSX",
        "Kenshi_WindowCX",
        NULL
    };
    MyGUI::Window* w = NULL;
    const char* chosen = NULL;
    for (const char** s = skins; *s && !w; ++s) {
        try {
            if (parent_or_gui_root_signal) {
                w = parent_or_gui_root_signal->createWidget<MyGUI::Window>(
                    *s, coord, align, name);
            } else {
                w = gui->createWidget<MyGUI::Window>(
                    *s, coord, align, layer, name);
            }
            chosen = *s;
        } catch (const MyGUI::Exception&) {
            w = NULL;
        }
    }
    if (w) {
        char dbg[128];
        _snprintf(dbg, sizeof(dbg),
            "[KenshiMP] browser: window '%s' using skin '%s'",
            name, chosen ? chosen : "(null)");
        KMP_LOG(dbg);
        w->setMovable(false);
        hide_window_chrome(w);  // no-op if skin has no chrome children
    }
    return w;
}

static void on_window_button(MyGUI::Window*, const std::string& name) {
    if (name == "close") server_browser_close();
}

static void on_add_window_button(MyGUI::Window*, const std::string& name) {
    if (name == "close" && s_add_window) s_add_window->setVisible(false);
}

static void on_refresh(MyGUI::Widget*) { start_all_pings(); }
static void on_back(MyGUI::Widget*)    { server_browser_close(); }

static void on_join(MyGUI::Widget*);  // forward-decl

// Manual double-click detection: MyGUI's eventMouseButtonDoubleClick
// doesn't fire on Button widgets with the Kenshi skin. Track the last
// clicked row-id + click time and call it a double when two hits on the
// same id land within 400 ms.
static std::string s_last_click_id;
static ULONGLONG   s_last_click_ms = 0;
static const ULONGLONG kDoubleClickMs = 400;

static void on_row_click(MyGUI::Widget* sender) {
    std::string id = sender->getUserString("kmp_row_id");
    if (id.empty()) return;

    ULONGLONG now_ms = GetTickCount64();
    bool is_double = (!s_last_click_id.empty()) && (s_last_click_id == id) &&
                     (now_ms - s_last_click_ms < kDoubleClickMs);

    s_selected_id = id;
    s_last_click_id = id;
    s_last_click_ms = now_ms;

    if (is_double) {
        if (s_pinger && s_pinger->status(id) == ServerPinger::Status::Success) {
            on_join(NULL);
            return;
        }
    }
    rebuild_rows();
    update_button_states();
}

static void on_add(MyGUI::Widget*) {
    if (!s_add_window) return;
    s_add_is_edit = false;
    s_add_is_direct = false;
    s_add_name->setCaption("");
    s_add_addr->setCaption("");
    s_add_pw->setCaption("");
    s_add_err->setCaption("");
    s_add_window->setCaption("Add Server");
    s_add_window->setVisible(true);
    MyGUI::LayerManager::getInstance().upLayerItem(s_add_window);
}

static void on_edit(MyGUI::Widget*) {
    if (!s_add_window || s_selected_id.empty()) return;
    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        if (e.id != s_selected_id) continue;
        s_add_is_edit = true;
        s_add_is_direct = false;
        s_add_name->setCaption(e.name);
        // Combine host:port into the single address field (skip ":port"
        // if it's the default 7777 — cleaner UX à la Minecraft).
        char abuf[256];
        if (e.port == 7777) {
            _snprintf(abuf, sizeof(abuf), "%s", e.address.c_str());
        } else {
            _snprintf(abuf, sizeof(abuf), "%s:%u",
                e.address.c_str(), static_cast<unsigned>(e.port));
        }
        s_add_addr->setCaption(abuf);
        s_add_pw->setCaption(e.password);
        s_add_err->setCaption("");
        s_add_window->setCaption("Edit Server");
        s_add_window->setVisible(true);
        MyGUI::LayerManager::getInstance().upLayerItem(s_add_window);
        return;
    }
}

static void on_remove(MyGUI::Widget*) {
    if (s_selected_id.empty()) return;
    for (std::vector<ServerEntry>::iterator it = s_entries.begin();
         it != s_entries.end(); ++it) {
        if (it->id != s_selected_id) continue;
        KMP_LOG(std::string("[KenshiMP] browser: remove '") + it->name + "'");
        s_entries.erase(it);
        break;
    }
    s_selected_id.clear();
    server_list_save(s_entries);
    rebuild_rows();
    update_button_states();
}

static void on_direct(MyGUI::Widget*) {
    if (!s_add_window) return;
    s_add_is_edit = false;
    s_add_is_direct = true;
    s_add_name->setCaption("");
    s_add_addr->setCaption("");
    s_add_pw->setCaption("");
    s_add_err->setCaption("");
    s_add_window->setCaption("Direct Connect");
    s_add_window->setVisible(true);
    MyGUI::LayerManager::getInstance().upLayerItem(s_add_window);
}

// Connecting placeholder modal. Real download+load wiring is Plan A.4.
static MyGUI::Window*  s_connecting_window = NULL;
static MyGUI::TextBox* s_connecting_label  = NULL;
static MyGUI::Button*  s_connecting_cancel = NULL;
static bool            s_connecting_visible = false;
static ULONGLONG       s_connecting_since_ms = 0;
static std::string     s_connecting_server_line;

static void on_connecting_cancel(MyGUI::Widget*);

static void update_connecting_caption() {
    if (!s_connecting_visible || !s_connecting_label) return;
    ULONGLONG elapsed = GetTickCount64() - s_connecting_since_ms;
    int dots = (int)((elapsed / 500) % 3) + 1;  // 1, 2, 3, 1, 2, 3, ...
    const char* dot_str = (dots == 1) ? "." : (dots == 2) ? ".." : "...";
    std::string caption = "Connecting" + std::string(dot_str) +
                          "\n\n" + s_connecting_server_line;
    s_connecting_label->setCaption(caption);
}

static void show_connecting_modal(const ServerEntry& e) {
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;

    if (!s_connecting_window) {
        s_connecting_window = create_chromeless_window(
            NULL, MyGUI::IntCoord(312, 280, 400, 200),
            MyGUI::Align::Default, "Overlapped", "KMP_ConnectingWindow");
        if (!s_connecting_window) return;
        s_connecting_window->setCaption("");
        s_connecting_label = s_connecting_window->createWidget<MyGUI::TextBox>(
            "Kenshi_TextBoxEmptySkin",
            MyGUI::IntCoord(16, 30, 368, 90), MyGUI::Align::Default);
        s_connecting_label->setFontName("Kenshi_StandardFont_Medium");
        s_connecting_label->setTextColour(MyGUI::Colour(0.92f, 0.88f, 0.82f));
        s_connecting_label->setTextAlign(MyGUI::Align::Center);

        s_connecting_cancel = s_connecting_window->createWidget<MyGUI::Button>(
            "Kenshi_Button1Skin",
            MyGUI::IntCoord(150, 130, 100, 32), MyGUI::Align::Default);
        s_connecting_cancel->setCaption("Cancel");
        s_connecting_cancel->setFontName("Kenshi_PaintedTextFont_Medium");
        s_connecting_cancel->setTextAlign(MyGUI::Align::Center);
        s_connecting_cancel->eventMouseButtonClick +=
            MyGUI::newDelegate(on_connecting_cancel);
    }

    char srv[192];
    _snprintf(srv, sizeof(srv), "%s\n%s:%u",
        e.name.c_str(), e.address.c_str(), static_cast<unsigned>(e.port));
    s_connecting_server_line = srv;
    s_connecting_since_ms = GetTickCount64();
    s_connecting_visible = true;
    update_connecting_caption();
    s_connecting_window->setVisible(true);
    MyGUI::LayerManager::getInstance().upLayerItem(s_connecting_window);

    // Disable everything on the main browser so only the Cancel button
    // responds while "connecting". Re-enabled when modal is dismissed.
    if (s_btn_refresh) s_btn_refresh->setEnabled(false);
    if (s_btn_back)    s_btn_back->setEnabled(false);
    if (s_btn_direct)  s_btn_direct->setEnabled(false);
    if (s_btn_add)     s_btn_add->setEnabled(false);
    if (s_btn_edit)    s_btn_edit->setEnabled(false);
    if (s_btn_remove)  s_btn_remove->setEnabled(false);
    if (s_btn_join)    s_btn_join->setEnabled(false);
    for (std::map<std::string, RowWidgets>::iterator it = s_rows.begin();
         it != s_rows.end(); ++it) {
        if (it->second.root) it->second.root->setEnabled(false);
    }
}

static void hide_connecting_modal() {
    if (s_connecting_window) s_connecting_window->setVisible(false);
    s_connecting_visible = false;

    // Restore main browser controls.
    if (s_btn_refresh) s_btn_refresh->setEnabled(true);
    if (s_btn_back)    s_btn_back->setEnabled(true);
    if (s_btn_direct)  s_btn_direct->setEnabled(true);
    if (s_btn_add)     s_btn_add->setEnabled(true);
    for (std::map<std::string, RowWidgets>::iterator it = s_rows.begin();
         it != s_rows.end(); ++it) {
        if (it->second.root) it->second.root->setEnabled(true);
    }
    update_button_states();  // Edit/Remove/Join based on selection again
}

static void on_connecting_cancel(MyGUI::Widget*) {
    KMP_LOG("[KenshiMP] Join cancelled by user");
    hide_connecting_modal();
}

static void on_join(MyGUI::Widget*) {
    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        if (e.id != s_selected_id) continue;
        char logbuf[256];
        _snprintf(logbuf, sizeof(logbuf),
            "[KenshiMP] Join clicked: '%s' @ %s:%u",
            e.name.c_str(), e.address.c_str(), static_cast<unsigned>(e.port));
        KMP_LOG(logbuf);
        show_connecting_modal(e);
        break;
    }
    // Don't close the browser; show the connecting modal on top instead.
}

// Get TitleScreen's main widget via BaseLayout::mMainWidget at offset 0x38.
static MyGUI::Widget* title_screen_root() {
    TitleScreen* ts = TitleScreen::getSingleton();
    if (!ts) return NULL;
    return *reinterpret_cast<MyGUI::Widget**>(
        reinterpret_cast<uint8_t*>(ts) + 0x38);
}

// Per-child visibility snapshot for the TitleScreen's menu children while
// the browser is open. We hide each child explicitly (setVisible on the
// parent doesn't cascade cleanly for all of Kenshi's menu widgets) and
// restore them on close.
struct TSChildSnap { MyGUI::Widget* w; bool was_visible; };
static std::vector<TSChildSnap> s_ts_hidden_children;

static void create_main_window() {
    if (s_window) return;
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;

    // Attach to the Gui root on the top-most MyGUI layer ("Pointer"), NOT
    // as child of TitleScreen. Using the Pointer layer puts us above
    // everything Kenshi renders, including the title-screen menu widgets
    // whose text captions render in a later pass. Falls back to
    // "Overlapped" if "Pointer" isn't registered.
    s_backdrop = NULL;

    // Attach as child of TitleScreen's main widget — this is the only
    // path that actually renders above Kenshi's title-screen background.
    // Gui-root on "Overlapped" is positioned BEHIND TitleScreen visually,
    // so even though the widget is correctly sized+visible, it's not seen.
    // If TitleScreen isn't available (e.g. opened mid-game later),
    // fall back to Gui root "Overlapped" which is normal for in-game
    // dialogs.
    // Full-screen window on Gui root, Overlapped layer. We hide
    // TitleScreen's main widget (and all its children) in open() before
    // showing the browser, so nothing from TitleScreen bleeds through.
    MyGUI::IntCoord full(0, 0, 1024, 768);
    s_window = create_chromeless_window(
        NULL, full, MyGUI::Align::Stretch, "Overlapped", "KMP_BrowserWindow");
    if (!s_window) return;
    s_window->setCaption("Multiplayer Servers");
    s_window->setVisible(false);
    s_window->eventWindowButtonPressed += MyGUI::newDelegate(on_window_button);

    const int pad = 6;
    s_btn_refresh = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(pad, pad, 90, 28),
        MyGUI::Align::Left | MyGUI::Align::Top);
    s_btn_refresh->setCaption("Refresh");
    s_btn_refresh->setFontName("Kenshi_PaintedTextFont_Medium");
    s_btn_refresh->setTextAlign(MyGUI::Align::Center);
    s_btn_refresh->eventMouseButtonClick += MyGUI::newDelegate(on_refresh);

    // Back button next to Refresh — closes the browser and returns to
    // the title screen.
    s_btn_back = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(pad + 96, pad, 90, 28),
        MyGUI::Align::Left | MyGUI::Align::Top);
    s_btn_back->setCaption("Back");
    s_btn_back->setFontName("Kenshi_PaintedTextFont_Medium");
    s_btn_back->setTextAlign(MyGUI::Align::Center);
    s_btn_back->eventMouseButtonClick += MyGUI::newDelegate(on_back);

    MyGUI::IntSize client = s_window->getClientCoord().size();
    // Plain Widget as container — rows are positioned explicitly from
    // y=0 down (no MyGUI auto-layout surprises). Scroll can be added
    // later as a wrapper ScrollView if the list grows long.
    MyGUI::IntCoord list_coord(pad, pad + 32,
                               client.width - 2 * pad,
                               client.height - 32 - 40 - 3 * pad);
    try {
        s_list_scroll = s_window->createWidget<MyGUI::Widget>(
            "Kenshi_TextBoxEmptySkin",
            list_coord, MyGUI::Align::Stretch);
    } catch (const MyGUI::Exception& e) {
        KMP_LOG(std::string("[KenshiMP] browser: list container failed: ") + e.what());
        s_list_scroll = NULL;
    }

    int by = client.height - 40 - pad;
    int bx = pad;
    s_btn_direct = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 110, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_direct->setCaption("Direct Connect");
    s_btn_direct->setFontName("Kenshi_PaintedTextFont_Medium");
    s_btn_direct->setTextAlign(MyGUI::Align::Center);
    s_btn_direct->eventMouseButtonClick += MyGUI::newDelegate(on_direct);
    bx += 116;
    s_btn_add = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 70, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_add->setCaption("Add");
    s_btn_add->setFontName("Kenshi_PaintedTextFont_Medium");
    s_btn_add->setTextAlign(MyGUI::Align::Center);
    s_btn_add->eventMouseButtonClick += MyGUI::newDelegate(on_add);
    bx += 76;
    s_btn_edit = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 70, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_edit->setCaption("Edit");
    s_btn_edit->setFontName("Kenshi_PaintedTextFont_Medium");
    s_btn_edit->setTextAlign(MyGUI::Align::Center);
    s_btn_edit->eventMouseButtonClick += MyGUI::newDelegate(on_edit);
    bx += 76;
    s_btn_remove = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 80, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_remove->setCaption("Remove");
    s_btn_remove->setFontName("Kenshi_PaintedTextFont_Medium");
    s_btn_remove->setTextAlign(MyGUI::Align::Center);
    s_btn_remove->eventMouseButtonClick += MyGUI::newDelegate(on_remove);
    bx += 86;
    s_btn_join = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 80, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_join->setCaption("Join");
    s_btn_join->setFontName("Kenshi_PaintedTextFont_Medium");
    s_btn_join->setTextAlign(MyGUI::Align::Center);
    s_btn_join->eventMouseButtonClick += MyGUI::newDelegate(on_join);
}

static void create_add_window() {
    if (s_add_window) return;
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;
    s_add_window = create_chromeless_window(
        NULL, MyGUI::IntCoord(150, 150, 320, 260),
        MyGUI::Align::Default, "Overlapped", "KMP_BrowserAddWindow");
    if (!s_add_window) return;
    s_add_window->setCaption("Add Server");
    s_add_window->setVisible(false);
    s_add_window->eventWindowButtonPressed += MyGUI::newDelegate(on_add_window_button);

    // Dark brown text colour matching the other in-game dialogs (see
    // ui.cpp). Default text on Kenshi_EditBox / Kenshi_TextBoxEmptySkin
    // is near-black on dark bg; we force a lighter colour for readability.
    MyGUI::Colour textCol(0.92f, 0.88f, 0.82f);

    const int pad = 8, lx = pad, lw = 80, fx = pad + 90, fw = 210, rowh = 28;
    int y = pad;
    struct L {
        static MyGUI::TextBox* mk(MyGUI::Window* w, int x, int yy, int ww, int hh,
                                  const char* cap, const MyGUI::Colour& col) {
            MyGUI::TextBox* t = w->createWidget<MyGUI::TextBox>(
                "Kenshi_TextBoxEmptySkin",
                MyGUI::IntCoord(x, yy, ww, hh), MyGUI::Align::Default);
            t->setCaption(cap);
            t->setFontName("Kenshi_StandardFont_Medium");
            t->setTextColour(col);
            return t;
        }
        static MyGUI::EditBox* mf(MyGUI::Window* w, int x, int yy, int ww, int hh,
                                  const MyGUI::Colour& col) {
            MyGUI::EditBox* e = w->createWidget<MyGUI::EditBox>(
                "Kenshi_EditBox",
                MyGUI::IntCoord(x, yy, ww, hh), MyGUI::Align::Default);
            e->setFontName("Kenshi_StandardFont_Medium");
            e->setTextColour(col);
            return e;
        }
    };
    L::mk(s_add_window, lx, y, lw, rowh, "Name:", textCol);
    s_add_name = L::mf(s_add_window, fx, y, fw, rowh, textCol); y += rowh + 4;
    L::mk(s_add_window, lx, y, lw, rowh, "Address:", textCol);
    s_add_addr = L::mf(s_add_window, fx, y, fw, rowh, textCol); y += rowh + 4;
    L::mk(s_add_window, lx, y, lw, rowh, "Password:", textCol);
    s_add_pw = L::mf(s_add_window, fx, y, fw, rowh, textCol); y += rowh + 8;

    s_add_err = s_add_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(pad, y, 300, rowh), MyGUI::Align::Default);
    s_add_err->setTextColour(MyGUI::Colour(1.0f, 0.3f, 0.3f));
    s_add_err->setCaption("");
    y += rowh + 4;

    s_add_ok = s_add_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(pad, y, 90, 30), MyGUI::Align::Default);
    s_add_ok->setCaption("OK");
    s_add_ok->setFontName("Kenshi_PaintedTextFont_Medium");
    s_add_ok->setTextAlign(MyGUI::Align::Center);
    s_add_ok->eventMouseButtonClick += MyGUI::newDelegate(on_add_ok);
    s_add_cancel = s_add_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(pad + 100, y, 90, 30), MyGUI::Align::Default);
    s_add_cancel->setCaption("Cancel");
    s_add_cancel->setFontName("Kenshi_PaintedTextFont_Medium");
    s_add_cancel->setTextAlign(MyGUI::Align::Center);
    s_add_cancel->eventMouseButtonClick += MyGUI::newDelegate(on_add_cancel);
}

// Parse "host" or "host:port" into (host, port). Returns false if the
// port portion is non-numeric or out of range. A missing ":port" yields
// the default 7777.
static bool parse_host_port(const std::string& in, std::string& host, uint16_t& port) {
    size_t colon = in.find(':');
    if (colon == std::string::npos) {
        host = in;
        port = 7777;
        return !host.empty();
    }
    host = in.substr(0, colon);
    std::string portstr = in.substr(colon + 1);
    if (host.empty() || portstr.empty()) return false;
    int p = std::atoi(portstr.c_str());
    if (p <= 0 || p > 65535) return false;
    port = static_cast<uint16_t>(p);
    return true;
}

static void on_add_ok(MyGUI::Widget*) {
    std::string name = s_add_name->getCaption();
    std::string addr_in = s_add_addr->getCaption();
    std::string pw = s_add_pw->getCaption();

    std::string host; uint16_t port = 7777;
    if (!s_add_is_direct && name.empty()) {
        s_add_err->setCaption("Name is required"); return;
    }
    if (addr_in.empty()) { s_add_err->setCaption("Address is required"); return; }
    if (!parse_host_port(addr_in, host, port)) {
        s_add_err->setCaption("Address must be host or host:port (1..65535)");
        return;
    }

    if (s_add_is_direct) {
        // No persistence — just trigger the Join flow and close.
        char logbuf[256];
        _snprintf(logbuf, sizeof(logbuf),
            "[KenshiMP] Direct Connect: %s:%u (password %s)",
            host.c_str(), static_cast<unsigned>(port),
            pw.empty() ? "(none)" : "provided");
        KMP_LOG(logbuf);
        if (s_add_window) s_add_window->setVisible(false);
        server_browser_close();
        return;
    }

    if (s_add_is_edit) {
        for (size_t i = 0; i < s_entries.size(); ++i) {
            ServerEntry& e = s_entries[i];
            if (e.id != s_selected_id) continue;
            e.name = name; e.address = host; e.port = port; e.password = pw;
            break;
        }
    } else {
        ServerEntry e;
        e.id = server_list_new_id();
        e.name = name; e.address = host; e.port = port; e.password = pw;
        s_entries.push_back(e);
        s_selected_id = e.id;
    }
    server_list_save(s_entries);
    s_add_window->setVisible(false);
    rebuild_rows();
    start_all_pings();
    update_button_states();
}

static void on_add_cancel(MyGUI::Widget*) {
    if (s_add_window) s_add_window->setVisible(false);
}

static float clock_seconds() {
    static LARGE_INTEGER freq;
    static LARGE_INTEGER t0;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        init = true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(now.QuadPart - t0.QuadPart) /
           static_cast<float>(freq.QuadPart);
}

static void start_all_pings() {
    stop_all_pings();
    if (s_entries.empty()) return;

    s_ping_host = enet_host_create(NULL,
        static_cast<size_t>(s_entries.size() + 1), 2, 0, 0);
    if (!s_ping_host) {
        KMP_LOG("[KenshiMP] browser: enet_host_create failed");
        return;
    }

    ServerPinger::Deps d;
    d.connect = [](const std::string& id,
                   const std::string& address, uint16_t port) -> bool {
        ENetAddress a;
        if (enet_address_set_host(&a, address.c_str()) != 0) return false;
        a.port = port;
        ENetPeer* peer = enet_host_connect(s_ping_host, &a, 2, 0);
        if (!peer) return false;
        s_peer_to_id[peer] = id;
        s_id_to_peer[id]   = peer;
        return true;
    };
    d.send_request = [](const std::string& id, uint32_t nonce) {
        std::map<std::string, ENetPeer*>::iterator it = s_id_to_peer.find(id);
        if (it == s_id_to_peer.end() || !it->second) return;
        ServerInfoRequest req; req.nonce = nonce;
        std::vector<uint8_t> buf = pack(req);
        ENetPacket* pkt = enet_packet_create(buf.data(), buf.size(),
                                             ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(it->second, 0, pkt);
        enet_host_flush(s_ping_host);
    };
    d.disconnect = [](const std::string& id) {
        std::map<std::string, ENetPeer*>::iterator it = s_id_to_peer.find(id);
        if (it == s_id_to_peer.end() || !it->second) return;
        enet_peer_disconnect_later(it->second, 0);
    };
    d.now_seconds = []() { return clock_seconds(); };
    s_pinger.reset(new ServerPinger(d));

    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        s_pinger->start(e.id, e.address, e.port);
        apply_ping_to_row(e.id);
    }
}

static void stop_all_pings() {
    if (s_pinger) s_pinger->clear();
    if (s_ping_host) {
        enet_host_flush(s_ping_host);
        enet_host_destroy(s_ping_host);
        s_ping_host = NULL;
    }
    s_peer_to_id.clear();
    s_id_to_peer.clear();
}

static void poll_ping_events() {
    if (!s_ping_host || !s_pinger) return;
    ENetEvent ev;
    while (enet_host_service(s_ping_host, &ev, 0) > 0) {
        std::map<ENetPeer*, std::string>::iterator it = s_peer_to_id.find(ev.peer);
        std::string id = (it == s_peer_to_id.end()) ? std::string() : it->second;
        if (ev.type == ENET_EVENT_TYPE_CONNECT && !id.empty()) {
            s_pinger->on_connected(id);
        } else if (ev.type == ENET_EVENT_TYPE_RECEIVE && !id.empty()) {
            PacketHeader h;
            if (peek_header(ev.packet->data, ev.packet->dataLength, h) &&
                h.type == PacketType::SERVER_INFO_REPLY) {
                ServerInfoReply reply;
                if (unpack(ev.packet->data, ev.packet->dataLength, reply)) {
                    ServerPinger::ReplyFields f;
                    f.player_count      = reply.player_count;
                    f.max_players       = reply.max_players;
                    f.protocol_version  = reply.protocol_version;
                    f.password_required = reply.password_required;
                    std::memcpy(f.description, reply.description, sizeof(f.description));
                    s_pinger->on_reply(id, reply.nonce, f);
                    apply_ping_to_row(id);
                    update_button_states();
                }
            }
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT && !id.empty()) {
            s_id_to_peer.erase(id);
            s_peer_to_id.erase(ev.peer);
        }
    }
    s_pinger->tick();
    for (size_t i = 0; i < s_entries.size(); ++i) {
        apply_ping_to_row(s_entries[i].id);
    }
}

static std::string format_ping_line(const ServerEntry& e) {
    if (!s_pinger) return "...";
    ServerPinger::Status::E st = s_pinger->status(e.id);
    const ServerPinger::Result& r = s_pinger->result(e.id);
    char buf[128];
    switch (st) {
    case ServerPinger::Status::Success:
        _snprintf(buf, sizeof(buf), "%u/%u   %ums   %s",
            r.player_count, r.max_players, r.ping_ms,
            r.password_required ? "LOCK" : "");
        return buf;
    case ServerPinger::Status::Connecting:     return "connecting...";
    case ServerPinger::Status::AwaitingReply:  return "waiting...";
    case ServerPinger::Status::DnsError:       return "- DNS error";
    case ServerPinger::Status::Offline:        return "- offline";
    case ServerPinger::Status::NoReply:        return "- no reply";
    case ServerPinger::Status::VersionMismatch:return "- version mismatch";
    default: return "...";
    }
}

static void rebuild_rows() {
    if (!s_list_scroll) return;
    for (std::map<std::string, RowWidgets>::iterator it = s_rows.begin();
         it != s_rows.end(); ++it) {
        if (it->second.root) MyGUI::Gui::getInstance().destroyWidget(it->second.root);
    }
    s_rows.clear();

    const int row_h = 44;
    int y = 0;
    int container_w = s_list_scroll->getCoord().width;
    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        RowWidgets rw;
        rw.root = s_list_scroll->createWidget<MyGUI::Button>(
            "Kenshi_Button1Skin",
            MyGUI::IntCoord(0, y, container_w, row_h),
            MyGUI::Align::HStretch | MyGUI::Align::Top);
        rw.root->setUserString("kmp_row_id", e.id);
        rw.root->setCaption("");
        rw.root->eventMouseButtonClick += MyGUI::newDelegate(on_row_click);
        if (e.id == s_selected_id) rw.root->setStateSelected(true);

        rw.line1 = rw.root->createWidget<MyGUI::TextBox>(
            "Kenshi_TextBoxEmptySkin",
            MyGUI::IntCoord(4, 2, 580, 20), MyGUI::Align::Default);
        rw.line1->setFontName("Kenshi_StandardFont_Medium");
        rw.line1->setCaption(e.name + "   -   " + format_ping_line(e));
        rw.line1->setNeedMouseFocus(false);  // clicks pass through to row Button

        rw.line2 = rw.root->createWidget<MyGUI::TextBox>(
            "Kenshi_TextBoxEmptySkin",
            MyGUI::IntCoord(4, 22, 580, 18), MyGUI::Align::Default);
        rw.line2->setFontName("Kenshi_StandardFont_Medium");
        rw.line2->setNeedMouseFocus(false);
        char subbuf[256];
        _snprintf(subbuf, sizeof(subbuf), "%s:%u",
            e.address.c_str(), static_cast<unsigned>(e.port));
        std::string sub = subbuf;
        if (s_pinger && s_pinger->status(e.id) == ServerPinger::Status::Success &&
            s_pinger->result(e.id).description[0]) {
            sub += "   \"";
            sub += s_pinger->result(e.id).description;
            sub += "\"";
        }
        rw.line2->setCaption(sub);

        s_rows[e.id] = rw;
        y += row_h + 2;
    }
    // No setCanvasSize — s_list_scroll is a plain Widget, not a ScrollView.
}

static void apply_ping_to_row(const std::string& id) {
    std::map<std::string, RowWidgets>::iterator it = s_rows.find(id);
    if (it == s_rows.end()) return;
    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        if (e.id != id) continue;
        if (it->second.line1) it->second.line1->setCaption(e.name + "   -   " + format_ping_line(e));
        return;
    }
}

static void update_button_states() {
    bool has_sel = false;
    bool can_join = false;
    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        if (e.id != s_selected_id) continue;
        has_sel = true;
        if (s_pinger && s_pinger->status(e.id) == ServerPinger::Status::Success) {
            can_join = true;
        }
        break;
    }
    if (s_btn_edit)   s_btn_edit->setEnabled(has_sel);
    if (s_btn_remove) s_btn_remove->setEnabled(has_sel);
    if (s_btn_join)   s_btn_join->setEnabled(can_join);
}

} // namespace

void server_browser_init() {
    // Deferred: window created lazily on first open.
}

void server_browser_shutdown() {
    stop_all_pings();
    if (s_pinger) s_pinger.reset();
}

void server_browser_open() {
    if (s_open) return;
    try {
        create_main_window();
    } catch (const MyGUI::Exception& e) {
        KMP_LOG(std::string("[KenshiMP] browser: create_main_window threw: ") + e.what());
        return;
    } catch (const std::exception& e) {
        KMP_LOG(std::string("[KenshiMP] browser: create_main_window std exception: ") + e.what());
        return;
    }
    if (!s_window) {
        KMP_LOG("[KenshiMP] browser: s_window null after create_main_window");
        return;
    }
    try {
        create_add_window();
    } catch (const MyGUI::Exception& e) {
        KMP_LOG(std::string("[KenshiMP] browser: create_add_window threw: ") + e.what());
        // Add window is optional — main window can still show.
    }

    s_entries.clear();
    server_list_load(s_entries);

    s_selected_id.clear();
    s_open = true;

    // Hide the entire TitleScreen widget so our Gui-root browser is the
    // only thing visible. Log each child we hide for diagnostic, so if
    // the user still sees TitleScreen captions we know those widgets
    // are outside this subtree and need separate handling.
    MyGUI::Widget* ts_root = title_screen_root();
    s_ts_hidden_children.clear();
    if (ts_root) {
        TSChildSnap root_snap;
        root_snap.w = ts_root;
        root_snap.was_visible = ts_root->getVisible();
        s_ts_hidden_children.push_back(root_snap);
        ts_root->setVisible(false);

        int counted = 0;
        MyGUI::EnumeratorWidgetPtr it = ts_root->getEnumerator();
        while (it.next()) {
            MyGUI::Widget* child = it.current();
            if (!child) continue;
            TSChildSnap snap;
            snap.w = child;
            snap.was_visible = child->getVisible();
            s_ts_hidden_children.push_back(snap);
            if (snap.was_visible) child->setVisible(false);
            counted++;
        }
        char dbg[96];
        _snprintf(dbg, sizeof(dbg),
            "[KenshiMP] browser: hid TitleScreen root + %d direct children",
            counted);
        KMP_LOG(dbg);
    }

    s_window->setVisible(true);
    try {
        MyGUI::LayerManager::getInstance().upLayerItem(s_window);
    } catch (...) { }

    // Log visibility / coord so we can verify rendering state.
    MyGUI::IntCoord c = s_window->getCoord();
    char dbg[128];
    _snprintf(dbg, sizeof(dbg),
        "[KenshiMP] browser: s_window visible=%d coord=%d,%d %dx%d",
        (int)s_window->getVisible(), c.left, c.top, c.width, c.height);
    KMP_LOG(dbg);

    rebuild_rows();
    update_button_states();
    start_all_pings();
}

void server_browser_close() {
    if (!s_open) return;
    // If we're mid-connect, Close is a no-op. User must hit Cancel in
    // the connecting modal first. This also means the X button on the
    // browser caption is effectively disabled while connecting.
    if (s_connecting_visible) {
        KMP_LOG("[KenshiMP] browser close blocked — connection in progress");
        return;
    }
    s_open = false;
    stop_all_pings();
    if (s_window) s_window->setVisible(false);
    if (s_backdrop) s_backdrop->setVisible(false);
    if (s_add_window) s_add_window->setVisible(false);
    hide_connecting_modal();

    // Restore TitleScreen root + children we hid (in reverse order so
    // parent goes back visible before children trigger re-render).
    for (size_t i = s_ts_hidden_children.size(); i > 0; --i) {
        MyGUI::Widget* w = s_ts_hidden_children[i - 1].w;
        if (w && s_ts_hidden_children[i - 1].was_visible) {
            w->setVisible(true);
        }
    }
    s_ts_hidden_children.clear();
}

bool server_browser_is_open() { return s_open; }

void server_browser_tick(float /*dt*/) {
    if (!s_open) return;
    poll_ping_events();
    update_connecting_caption();  // animates "Connecting." -> ".." -> "..."

    // Keep the connecting modal permanently frontmost. Clicking the main
    // browser (even disabled) can raise its window above the modal
    // otherwise — this forces the modal back on top every tick.
    if (s_connecting_visible && s_connecting_window) {
        try {
            MyGUI::LayerManager::getInstance().upLayerItem(s_connecting_window);
        } catch (...) { }
    }
}

} // namespace kmp
