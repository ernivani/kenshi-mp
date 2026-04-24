#include "char_editor.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <ogre/OgreMemorySTLAllocator.h>

#include <MyGUI.h>

#include <kenshi/Character.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/util/lektor.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/CharacterEditWindow.h>
#include <core/Functions.h>  // KenshiLib::AddHook, GetRealAddress

#include <vector>

#include "kmp_log.h"
#include "re_tools.h"

// Kenshi's KenshiLib stub does NOT export GameDataReference::~GameDataReference,
// yet MSVC instantiates std::vector<GameDataReference>::~vector() which
// references it — even if the vector is always empty and the destructor
// never fires at runtime. Provide an empty local definition just to
// satisfy the linker. Never invoked (we only use empty vectors).
GameDataReference::~GameDataReference() {}

namespace kmp {

// Captured ForgottenGUI instance — set by our hook on the first call
// to ForgottenGUI::update(). Remains valid for the rest of the session.
static ForgottenGUI* s_fgui = NULL;

// Trampoline pointer set up by KenshiLib::AddHook — points to the
// original ForgottenGUI::update (after our hook takes over the stub).
static void (*s_orig_fgui_update)(ForgottenGUI*) = NULL;

static void hooked_fgui_update(ForgottenGUI* self) {
    if (!s_fgui) {
        s_fgui = self;
        char buf[96];
        _snprintf(buf, sizeof(buf),
            "[KenshiMP] ForgottenGUI captured at %p", (void*)self);
        KMP_LOG(buf);
    }
    if (s_orig_fgui_update) s_orig_fgui_update(self);
}

// Hook into CharacterEditWindow::_CONSTRUCTOR so we can see whether the
// editor actually gets instantiated when we invoke showCharacterEditor.
// If this never fires on our call path → showCharacterEditor is failing
// BEFORE it gets to construct the window. If it fires and then hangs →
// the hang is inside the ctor (loadData / setupUI).
typedef CharacterEditWindow* (*CEW_Ctor_t)(
    CharacterEditWindow* self,
    const lektor<Character*>& chars,
    CharacterEditMode mode,
    const void* races);
static CEW_Ctor_t s_orig_cew_ctor = NULL;

static CharacterEditWindow* hooked_cew_ctor(
    CharacterEditWindow* self,
    const lektor<Character*>& chars,
    CharacterEditMode mode,
    const void* races) {
    char buf[160];
    _snprintf(buf, sizeof(buf),
        "[KenshiMP] CEW::ctor ENTER self=%p chars.count=%u mode=%d races=%p",
        (void*)self, chars.count, (int)mode, races);
    KMP_LOG(buf);
    CharacterEditWindow* result =
        s_orig_cew_ctor ? s_orig_cew_ctor(self, chars, mode, races)
                        : self;
    _snprintf(buf, sizeof(buf),
        "[KenshiMP] CEW::ctor LEAVE result=%p", (void*)result);
    KMP_LOG(buf);
    return result;
}

// Track which editor instance is "ours" so we only intercept confirm
// for our spawned editor, not for any legitimate future new-game use.
static CharacterEditWindow* s_our_editor = NULL;

typedef void (*CEW_ConfirmBtn_t)(CharacterEditWindow* self, MyGUI::Widget* sender);
static CEW_ConfirmBtn_t s_orig_confirm_btn = NULL;

static void restore_hud_and_close_editor(CharacterEditWindow* self) {
    s_our_editor = NULL;

    // Unlink from FGUI FIRST so any game-thread update() that fires
    // during destruction doesn't touch the dying object.
    if (s_fgui) {
        uint8_t* base = reinterpret_cast<uint8_t*>(s_fgui);
        CharacterEditWindow** slot =
            reinterpret_cast<CharacterEditWindow**>(base + 0x1C0);
        if (*slot == self) *slot = NULL;
        KMP_LOG("[KenshiMP] char_editor: unlinked from fgui+0x1C0");
    }

    // Call CharacterEditWindow::_DESTRUCTOR directly. This destroys the
    // MyGUI widgets (via BaseLayout's dtor) without going through
    // ForgottenGUI::closeCharacterEditor, which hangs in our context.
    typedef void (*DtorFn)(CharacterEditWindow* self);
    uintptr_t dtor_addr = re_resolve_symbol(
        "?_DESTRUCTOR@CharacterEditWindow@@QEAAXXZ");
    if (dtor_addr && self) {
        KMP_LOG("[KenshiMP] char_editor: calling CEW::_DESTRUCTOR");
        reinterpret_cast<DtorFn>(dtor_addr)(self);
        KMP_LOG("[KenshiMP] char_editor: CEW::_DESTRUCTOR returned");
    } else {
        KMP_LOG("[KenshiMP] char_editor: CEW::_DESTRUCTOR unresolved");
    }

    // Re-show the main HUD.
    typedef void (*ShowMainbarFn)(ForgottenGUI* self, bool on);
    uintptr_t addr = re_resolve_symbol(
        "?showMainbar@ForgottenGUI@@QEAAX_N@Z");
    if (addr && s_fgui) {
        reinterpret_cast<ShowMainbarFn>(addr)(s_fgui, true);
    }
    if (ou && ou->player) ou->player->setCharacterEditMode(false);
    KMP_LOG("[KenshiMP] char_editor: HUD restored, edit mode off");
}

static void hooked_confirm_btn(CharacterEditWindow* self, MyGUI::Widget* sender) {
    if (self == s_our_editor) {
        KMP_LOG("[KenshiMP] char_editor: confirmButton INTERCEPTED — "
                "skipping new-game callback, doing our own close");
        restore_hud_and_close_editor(self);
        return;  // DO NOT call original — it would trigger new-game flow
    }
    // Not ours — let Kenshi's normal flow run.
    if (s_orig_confirm_btn) s_orig_confirm_btn(self, sender);
}

void char_editor_install_hook() {
    re_tools_init();

    // Hook FGUI::update to capture the singleton.
    uintptr_t fgui_update = re_resolve_symbol(
        "?update@ForgottenGUI@@QEAAXXZ");
    if (fgui_update) {
        if (re_hook(fgui_update, &hooked_fgui_update,
                    reinterpret_cast<void**>(&s_orig_fgui_update))) {
            KMP_LOG("[KenshiMP] char_editor: hooked FGUI::update");
        }
    } else {
        KMP_LOG("[KenshiMP] char_editor: FGUI::update symbol not found");
    }

    // Hook CharacterEditWindow::_CONSTRUCTOR for diagnostics.
    uintptr_t cew_ctor = re_resolve_symbol(
        "?_CONSTRUCTOR@CharacterEditWindow@@QEAAPEAV1@AEBV?$lektor@"
        "PEAVCharacter@@@@W4CharacterEditMode@@PEBV?$vector@"
        "VGameDataReference@@V?$STLAllocator@VGameDataReference@@V?$"
        "CategorisedAllocPolicy@$0A@@Ogre@@@Ogre@@@std@@@Z");
    if (cew_ctor) {
        if (re_hook(cew_ctor, &hooked_cew_ctor,
                    reinterpret_cast<void**>(&s_orig_cew_ctor))) {
            KMP_LOG("[KenshiMP] char_editor: hooked CEW::_CONSTRUCTOR");
        }
    } else {
        KMP_LOG("[KenshiMP] char_editor: CEW::_CONSTRUCTOR symbol not found");
    }

    // Hook confirmButton so we can intercept the user's "confirm" click
    // on our spawned editor and skip the original callback (which would
    // try to invoke the native new-game flow and crash).
    uintptr_t cew_confirm = re_resolve_symbol(
        "?confirmButton@CharacterEditWindow@@QEAAXPEAVWidget@MyGUI@@@Z");
    if (cew_confirm) {
        if (re_hook(cew_confirm, &hooked_confirm_btn,
                    reinterpret_cast<void**>(&s_orig_confirm_btn))) {
            KMP_LOG("[KenshiMP] char_editor: hooked CEW::confirmButton");
        }
    } else {
        KMP_LOG("[KenshiMP] char_editor: CEW::confirmButton symbol not found");
    }
}

bool char_editor_ready() { return s_fgui != NULL; }

bool char_editor_is_open() { return s_our_editor != NULL; }

static Character* s_pending_char = NULL;
static int        s_pending_delay_frames = 0;

void char_editor_open_deferred(Character* ch) {
    s_pending_char = ch;
    // Wait 30 frames (~0.5 s at 60 fps) so CONNECT_ACCEPT + destroy +
    // spawn have fully drained before we drop the editor into the
    // render path.
    s_pending_delay_frames = 30;
    KMP_LOG("[KenshiMP] char_editor: deferred open scheduled");
}

void char_editor_tick() {
    if (!s_pending_char) return;
    if (s_pending_delay_frames > 0) {
        --s_pending_delay_frames;
        return;
    }
    Character* ch = s_pending_char;
    s_pending_char = NULL;
    char_editor_open(ch);
}

// Kenshi's showCharacterEditor signature. Last arg is a pointer to an
// Ogre-allocator vector of GameDataReference — we pass an empty vector
// (not NULL!) so the editor's own loadData path populates races from
// gamedata. Passing NULL is what PlayerInterface::activateCharacterEditMode
// does internally, and it crashes at kenshi_x64.exe+0x7F678B.
typedef std::vector<GameDataReference,
    Ogre::STLAllocator<GameDataReference,
        Ogre::CategorisedAllocPolicy<Ogre::MEMCATEGORY_GENERAL> > > RacesVec;

typedef void (*ShowCharEditFn)(ForgottenGUI* self,
                               lektor<Character*> chars,
                               CharacterEditMode mode,
                               const RacesVec* races);

static ShowCharEditFn s_show_char_edit = NULL;


static ShowCharEditFn resolve_show_char_edit() {
    if (s_show_char_edit) return s_show_char_edit;
    HMODULE klib = GetModuleHandleA("KenshiLib.dll");
    if (!klib) return NULL;
    void* stub = (void*)GetProcAddress(klib,
        "?showCharacterEditor@ForgottenGUI@@QEAAXV?$lektor@PEAVCharacter@@@@"
        "W4CharacterEditMode@@PEBV?$vector@VGameDataReference@@V?$STLAllocator"
        "@VGameDataReference@@V?$CategorisedAllocPolicy@$0A@@Ogre@@@Ogre@@@std@@@Z");
    if (!stub) return NULL;
    intptr_t addr = KenshiLib::GetRealAddress(stub);
    s_show_char_edit = reinterpret_cast<ShowCharEditFn>(addr);
    return s_show_char_edit;
}

void char_editor_open(Character* ch) {
    if (!s_fgui) {
        KMP_LOG("[KenshiMP] char_editor: not ready (FGUI not captured yet)");
        return;
    }
    if (!ch) {
        KMP_LOG("[KenshiMP] char_editor: null character");
        return;
    }
    ShowCharEditFn fn = resolve_show_char_edit();
    if (!fn) {
        KMP_LOG("[KenshiMP] char_editor: showCharacterEditor symbol unresolved");
        return;
    }

    // Build a single-element lektor<Character*>. lektor doesn't own its
    // storage (it's just {count, maxSize, stuff}) so a stack-allocated
    // array is fine here — the editor copies what it needs immediately.
    Character* buf[1] = { ch };
    lektor<Character*> chars;
    chars.count   = 1;
    chars.maxSize = 1;
    chars.stuff   = buf;

    // Empty races vector — let the editor populate from gamedata.
    RacesVec races_empty;

    // Flip PlayerInterface into character-edit mode first.
    if (ou && ou->player) {
        ou->player->setCharacterEditMode(true);
        KMP_LOG("[KenshiMP] char_editor: PlayerInterface.editMode=true");
    }

    // Bypass ForgottenGUI::showCharacterEditor (which hangs post-ctor)
    // and construct CharacterEditWindow directly via its exported ctor.
    // The ctor fires our instrumentation hook (ENTER/LEAVE logs) and
    // returns normally — so the issue is in the surrounding wrapper.
    // We allocate raw storage, call _CONSTRUCTOR, then store the
    // pointer into fgui->characterEditor (offset 0x1C0) so the rest of
    // the GUI sees the editor as live.
    typedef CharacterEditWindow* (*CEW_Ctor_t)(
        CharacterEditWindow* self,
        const lektor<Character*>& chars,
        CharacterEditMode mode,
        const RacesVec* races);
    uintptr_t ctor_addr = re_resolve_symbol(
        "?_CONSTRUCTOR@CharacterEditWindow@@QEAAPEAV1@AEBV?$lektor@"
        "PEAVCharacter@@@@W4CharacterEditMode@@PEBV?$vector@"
        "VGameDataReference@@V?$STLAllocator@VGameDataReference@@V?$"
        "CategorisedAllocPolicy@$0A@@Ogre@@@Ogre@@@std@@@Z");
    if (!ctor_addr) {
        KMP_LOG("[KenshiMP] char_editor: CEW::_CONSTRUCTOR unresolved, falling back");
        KMP_LOG("[KenshiMP] char_editor: calling showCharacterEditor (EDIT_DEBUG)");
        fn(s_fgui, chars, EDIT_DEBUG, &races_empty);
        KMP_LOG("[KenshiMP] char_editor: showCharacterEditor returned OK");
        return;
    }

    // Size of CharacterEditWindow per header: members go up to ~0x438.
    // Round up to a generous 0x500 so we don't underflow. The object
    // leaks intentionally — the editor owns itself once alive.
    const size_t kCEWSize = 0x500;
    void* mem = ::operator new(kCEWSize);
    std::memset(mem, 0, kCEWSize);
    CharacterEditWindow* win = reinterpret_cast<CharacterEditWindow*>(mem);

    CEW_Ctor_t ctor = reinterpret_cast<CEW_Ctor_t>(ctor_addr);
    {
        char lb[96];
        _snprintf(lb, sizeof(lb),
            "[KenshiMP] char_editor: direct ctor mem=%p", mem);
        KMP_LOG(lb);
    }
    CharacterEditWindow* result = ctor(win, chars, EDIT_DEBUG, &races_empty);
    {
        char lb[96];
        _snprintf(lb, sizeof(lb),
            "[KenshiMP] char_editor: direct ctor returned result=%p", result);
        KMP_LOG(lb);
    }

    // Publish into ForgottenGUI at offset 0x1C0 so the rest of the
    // game treats the editor as live (isCharacterEditorMode(), update
    // dispatch, etc.).
    if (s_fgui && result) {
        uint8_t* base = reinterpret_cast<uint8_t*>(s_fgui);
        CharacterEditWindow** slot =
            reinterpret_cast<CharacterEditWindow**>(base + 0x1C0);
        *slot = result;
        KMP_LOG("[KenshiMP] char_editor: published editor into fgui+0x1C0");
    }

    // Mark this instance as OURS so the confirmButton hook intercepts
    // its click (and skips the destructive new-game callback).
    s_our_editor = result;

    // Hide the in-game HUD so the editor isn't visually buried under
    // the main bar + message log. Use ForgottenGUI::showMainbar(false).
    {
        typedef void (*ShowMainbarFn)(ForgottenGUI* self, bool on);
        uintptr_t addr = re_resolve_symbol(
            "?showMainbar@ForgottenGUI@@QEAAX_N@Z");
        if (addr && s_fgui) {
            ShowMainbarFn fn2 = reinterpret_cast<ShowMainbarFn>(addr);
            fn2(s_fgui, false);
            KMP_LOG("[KenshiMP] char_editor: HUD main bar hidden");
        }
    }

    // Note: previously tried to upLayerItem on *(editor+0x38) assuming
    // BaseLayout::mMainWidget, but CharacterEditWindow layout differs
    // (it inherits wraps::BaseLayout which is at a different offset
    // inside the full object) — reading 0x38 crashes Kenshi. The ctor's
    // setupUI() has already attached widgets to their layers; hiding
    // the HUD + MyGUI's natural z-order should be enough.
    KMP_LOG("[KenshiMP] char_editor: editor construction complete");
}

} // namespace kmp
