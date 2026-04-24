// char_editor.h — Capture ForgottenGUI* at runtime so we can call
// Kenshi's native character editor (showCharacterEditor) on our own
// terms. PlayerInterface::activateCharacterEditMode takes a shortcut
// that crashes in mid-game (races=NULL), so we reach into FGUI directly.
#pragma once

class Character;

namespace kmp {

// Install the ForgottenGUI::update hook. Called once from startPlugin.
// After the first frame that calls FGUI::update, the singleton pointer
// is captured and char_editor_open() can be used.
void char_editor_install_hook();

// True once ForgottenGUI::update has fired at least once (pointer live).
bool char_editor_ready();

// True while our editor is open (between char_editor_open and the
// intercepted confirmButton). Other subsystems (player_sync, host_sync)
// may use this to suppress packet sends during edit mode.
bool char_editor_is_open();

// Open Kenshi's native character editor on `ch` immediately. No-op if
// char_editor_ready() is false. Safe to call repeatedly.
void char_editor_open(Character* ch);

// Queue a deferred open. The actual showCharacterEditor call happens in
// char_editor_tick() two ticks later, so it runs from a clean game
// frame instead of from inside a packet handler / CONNECT_ACCEPT chain.
void char_editor_open_deferred(Character* ch);

// Called every frame from hooked_main_loop. Drives the deferred-open
// state machine.
void char_editor_tick();

} // namespace kmp
