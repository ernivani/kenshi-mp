// server_browser.h — MyGUI modal listing saved KenshiMP servers with
// live ping + player count + description. See spec
// docs/superpowers/specs/2026-04-22-server-browser-ui-design.md
#pragma once

namespace kmp {

void server_browser_init();
void server_browser_shutdown();
void server_browser_open();
void server_browser_close();
void server_browser_tick(float dt);
bool server_browser_is_open();

} // namespace kmp
