// server_browser.cpp — see server_browser.h
#include "server_browser.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <MyGUI.h>
#include <OgreLogManager.h>

#include <enet/enet.h>

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
static MyGUI::Window*                s_window = NULL;
static MyGUI::Button*                s_btn_refresh = NULL;
static MyGUI::Button*                s_btn_direct  = NULL;
static MyGUI::Button*                s_btn_add     = NULL;
static MyGUI::Button*                s_btn_edit    = NULL;
static MyGUI::Button*                s_btn_remove  = NULL;
static MyGUI::Button*                s_btn_join    = NULL;
static MyGUI::ScrollView*            s_list_scroll = NULL;
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
static MyGUI::EditBox*  s_add_addr   = NULL;
static MyGUI::EditBox*  s_add_port   = NULL;
static MyGUI::EditBox*  s_add_pw     = NULL;
static MyGUI::TextBox*  s_add_err    = NULL;
static MyGUI::Button*   s_add_ok     = NULL;
static MyGUI::Button*   s_add_cancel = NULL;
static bool             s_add_is_edit = false;

static void rebuild_rows();
static void update_button_states();
static void start_all_pings();
static void stop_all_pings();
static void apply_ping_to_row(const std::string& id);
static void on_add_ok(MyGUI::Widget*);
static void on_add_cancel(MyGUI::Widget*);

static void on_window_button(MyGUI::Window*, const std::string& name) {
    if (name == "close") server_browser_close();
}

static void on_add_window_button(MyGUI::Window*, const std::string& name) {
    if (name == "close" && s_add_window) s_add_window->setVisible(false);
}

static void on_refresh(MyGUI::Widget*) { start_all_pings(); }

static void on_row_click(MyGUI::Widget* sender) {
    std::string id = sender->getUserString("kmp_row_id");
    if (id.empty()) return;
    s_selected_id = id;
    rebuild_rows();
    update_button_states();
}

static void on_add(MyGUI::Widget*) {
    if (!s_add_window) return;
    s_add_is_edit = false;
    s_add_name->setCaption("");
    s_add_addr->setCaption("");
    s_add_port->setCaption("7777");
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
        s_add_name->setCaption(e.name);
        s_add_addr->setCaption(e.address);
        char pbuf[16];
        _snprintf(pbuf, sizeof(pbuf), "%u", static_cast<unsigned>(e.port));
        s_add_port->setCaption(pbuf);
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
    // MVP: same as Add. User deletes after if they don't want persisted.
    on_add(NULL);
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
        break;
    }
    server_browser_close();
}

static void create_main_window() {
    if (s_window) return;
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;

    s_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_WindowCX",
        MyGUI::IntCoord(100, 100, 600, 460),
        MyGUI::Align::Default,
        "Overlapped",
        "KMP_BrowserWindow"
    );
    s_window->setCaption("Multiplayer Servers");
    s_window->setVisible(false);
    s_window->eventWindowButtonPressed += MyGUI::newDelegate(on_window_button);

    const int pad = 6;
    s_btn_refresh = s_window->createWidget<MyGUI::Button>(
        "Kenshi_Button1Skin",
        MyGUI::IntCoord(pad, pad, 90, 28),
        MyGUI::Align::Left | MyGUI::Align::Top);
    s_btn_refresh->setCaption("Refresh");
    s_btn_refresh->eventMouseButtonClick += MyGUI::newDelegate(on_refresh);

    MyGUI::IntSize client = s_window->getClientCoord().size();
    s_list_scroll = s_window->createWidget<MyGUI::ScrollView>(
        "Kenshi_ScrollViewSkin",
        MyGUI::IntCoord(pad, pad + 32,
                        client.width - 2 * pad,
                        client.height - 32 - 40 - 3 * pad),
        MyGUI::Align::Stretch);
    s_list_scroll->setVisibleHScroll(false);
    s_list_scroll->setVisibleVScroll(true);

    int by = client.height - 40 - pad;
    int bx = pad;
    s_btn_direct = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 110, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_direct->setCaption("Direct Connect");
    s_btn_direct->eventMouseButtonClick += MyGUI::newDelegate(on_direct);
    bx += 116;
    s_btn_add = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 70, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_add->setCaption("Add");
    s_btn_add->eventMouseButtonClick += MyGUI::newDelegate(on_add);
    bx += 76;
    s_btn_edit = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 70, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_edit->setCaption("Edit");
    s_btn_edit->eventMouseButtonClick += MyGUI::newDelegate(on_edit);
    bx += 76;
    s_btn_remove = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 80, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_remove->setCaption("Remove");
    s_btn_remove->eventMouseButtonClick += MyGUI::newDelegate(on_remove);
    bx += 86;
    s_btn_join = s_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(bx, by, 80, 32), MyGUI::Align::Left | MyGUI::Align::Bottom);
    s_btn_join->setCaption("Join");
    s_btn_join->eventMouseButtonClick += MyGUI::newDelegate(on_join);
}

static void create_add_window() {
    if (s_add_window) return;
    MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
    if (!gui) return;
    s_add_window = gui->createWidget<MyGUI::Window>(
        "Kenshi_WindowCX",
        MyGUI::IntCoord(150, 150, 320, 260),
        MyGUI::Align::Default, "Overlapped", "KMP_BrowserAddWindow");
    s_add_window->setCaption("Add Server");
    s_add_window->setVisible(false);
    s_add_window->eventWindowButtonPressed += MyGUI::newDelegate(on_add_window_button);

    const int pad = 8, lx = pad, lw = 80, fx = pad + 90, fw = 210, rowh = 28;
    int y = pad;
    struct L {
        static MyGUI::TextBox* mk(MyGUI::Window* w, int x, int yy, int ww, int hh,
                                  const char* cap) {
            MyGUI::TextBox* t = w->createWidget<MyGUI::TextBox>(
                "Kenshi_TextBoxEmptySkin",
                MyGUI::IntCoord(x, yy, ww, hh), MyGUI::Align::Default);
            t->setCaption(cap);
            t->setFontName("Kenshi_StandardFont_Medium");
            return t;
        }
        static MyGUI::EditBox* mf(MyGUI::Window* w, int x, int yy, int ww, int hh) {
            return w->createWidget<MyGUI::EditBox>(
                "Kenshi_EditBox",
                MyGUI::IntCoord(x, yy, ww, hh), MyGUI::Align::Default);
        }
    };
    L::mk(s_add_window, lx, y, lw, rowh, "Name:");
    s_add_name = L::mf(s_add_window, fx, y, fw, rowh); y += rowh + 4;
    L::mk(s_add_window, lx, y, lw, rowh, "Address:");
    s_add_addr = L::mf(s_add_window, fx, y, fw, rowh); y += rowh + 4;
    L::mk(s_add_window, lx, y, lw, rowh, "Port:");
    s_add_port = L::mf(s_add_window, fx, y, fw, rowh); y += rowh + 4;
    L::mk(s_add_window, lx, y, lw, rowh, "Password:");
    s_add_pw = L::mf(s_add_window, fx, y, fw, rowh); y += rowh + 8;

    s_add_err = s_add_window->createWidget<MyGUI::TextBox>(
        "Kenshi_TextBoxEmptySkin",
        MyGUI::IntCoord(pad, y, 300, rowh), MyGUI::Align::Default);
    s_add_err->setTextColour(MyGUI::Colour(1.0f, 0.3f, 0.3f));
    s_add_err->setCaption("");
    y += rowh + 4;

    s_add_ok = s_add_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(pad, y, 90, 30), MyGUI::Align::Default);
    s_add_ok->setCaption("OK");
    s_add_ok->eventMouseButtonClick += MyGUI::newDelegate(on_add_ok);
    s_add_cancel = s_add_window->createWidget<MyGUI::Button>("Kenshi_Button1Skin",
        MyGUI::IntCoord(pad + 100, y, 90, 30), MyGUI::Align::Default);
    s_add_cancel->setCaption("Cancel");
    s_add_cancel->eventMouseButtonClick += MyGUI::newDelegate(on_add_cancel);
}

static void on_add_ok(MyGUI::Widget*) {
    std::string name = s_add_name->getCaption();
    std::string addr = s_add_addr->getCaption();
    std::string portstr = s_add_port->getCaption();
    std::string pw = s_add_pw->getCaption();
    if (name.empty()) { s_add_err->setCaption("Name is required"); return; }
    if (addr.empty()) { s_add_err->setCaption("Address is required"); return; }
    int port = std::atoi(portstr.c_str());
    if (port <= 0 || port > 65535) { s_add_err->setCaption("Port must be 1..65535"); return; }

    if (s_add_is_edit) {
        for (size_t i = 0; i < s_entries.size(); ++i) {
            ServerEntry& e = s_entries[i];
            if (e.id != s_selected_id) continue;
            e.name = name; e.address = addr;
            e.port = static_cast<uint16_t>(port); e.password = pw;
            break;
        }
    } else {
        ServerEntry e;
        e.id = server_list_new_id();
        e.name = name; e.address = addr;
        e.port = static_cast<uint16_t>(port); e.password = pw;
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
    for (size_t i = 0; i < s_entries.size(); ++i) {
        const ServerEntry& e = s_entries[i];
        RowWidgets rw;
        rw.root = s_list_scroll->createWidget<MyGUI::Button>(
            "Kenshi_Button1Skin",
            MyGUI::IntCoord(0, y, s_list_scroll->getViewCoord().width, row_h),
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

        rw.line2 = rw.root->createWidget<MyGUI::TextBox>(
            "Kenshi_TextBoxEmptySkin",
            MyGUI::IntCoord(4, 22, 580, 18), MyGUI::Align::Default);
        rw.line2->setFontName("Kenshi_StandardFont_Medium");
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
    s_list_scroll->setCanvasSize(s_list_scroll->getViewCoord().width, y);
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
    create_main_window();
    if (!s_window) return;
    create_add_window();

    s_entries.clear();
    server_list_load(s_entries);

    s_selected_id.clear();
    s_open = true;
    s_window->setVisible(true);
    MyGUI::LayerManager::getInstance().upLayerItem(s_window);
    rebuild_rows();
    update_button_states();
    start_all_pings();
}

void server_browser_close() {
    if (!s_open) return;
    s_open = false;
    stop_all_pings();
    if (s_window) s_window->setVisible(false);
    if (s_add_window) s_add_window->setVisible(false);
}

bool server_browser_is_open() { return s_open; }

void server_browser_tick(float /*dt*/) {
    if (!s_open) return;
    poll_ping_events();
}

} // namespace kmp
