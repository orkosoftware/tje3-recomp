// Additions to the game's own menus, built with the game's menu code so they
// look and sound like the rest:
//   - "Exit Game" at the bottom of the main menu (the Xbox had a power button);
//   - "Display Settings" at the bottom of Options: a page of picture options
//     that apply while the game runs. A changes the selected one, B goes back.
// The highlight also wraps around: up from the first entry goes to the last, and
// down from the last to the first.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../gpu/gpu.h"
#include "../gpu/window.h"
#include "../guest.h"
#include "../settings.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace tje {

extern FILE* g_log;

namespace {

const uint32_t kOptionsLabel = 0x1d04b8;        // "Options", the main menu's last entry
const uint32_t kDownloadLabel = 0x1d04c0;       // "Download Content", the Options page's first entry
const uint32_t kPlayerOptionsLabel = 0x1d04e8;  // "Player Options", its last
const uint32_t kMainMenuPage = 1, kOptionsPage = 5;
const uint32_t kMenuItem = 0x40, kMenuPage = 0x80;      // menu: selected item, page shown
const uint32_t kOptionsCursor = 0x98;                   // menu: selected row of the Options page
const uint32_t kItemLabel = 0x54, kItemTextX = 0xd4;    // item: its text, and where the text starts
const uint32_t kFontGlobal = 0x23c658;
const uint32_t kSelectReturn = 0x57aac;         // where the menu's "A pressed" code calls its sound
const char kExitText[] = "Exit Game";
const char kSettingsText[] = "Display Settings";

struct Choice { int value; const char* name; };
struct Row {
    const char* key;
    const char* title;
    const Choice* choices;
    int count;
};

const Choice kDisplay[] = {{0, "Original 4:3"}, {1, "Widescreen"}, {2, "Stretch"}, {3, "Sharp Pixels"}};
const Choice kResolution[] = {{0, "Match Screen"}, {1, "Original"}, {2, "2x"}, {3, "3x"}, {4, "4x"}};
const Choice kOnOff[] = {{1, "On"}, {0, "Off"}};
const Choice kCamera[] = {{1, "Modern"}, {0, "Original"}};

const Row kRows[] = {
    {"display_mode", "Display", kDisplay, 4},
    {"resolution", "Resolution", kResolution, 5},
    {"smoothing", "Smoothing", kOnOff, 2},
#ifdef __ANDROID__
    {"invert_camera_x", "Camera Turn", kCamera, 2},
#else
    {"vsync", "V-Sync", kOnOff, 2},
    {"fullscreen", "Fullscreen", kOnOff, 2},
#endif
};
const int kRowCount = (int)(sizeof kRows / sizeof kRows[0]);

enum class Page { None, Opening, Shown };
Page g_page = Page::None;
uint32_t g_menu = 0;                // the menu showing (or about to show) the settings page
uint32_t g_items[8] = {};           // its items, by row

int choice_index(const Row& r) {
    const int v = setting_int(r.key);
    for (int i = 0; i < r.count; i++)
        if (r.choices[i].value == v) return i;
    return 0;
}

// A row's text in guest memory ("Display: Widescreen").
uint32_t row_label(int row) {
    static const uint32_t buf = guest_alloc(8 * 128);
    const Row& r = kRows[row];
    char* p = gptr(buf + row * 128);
    std::snprintf(p, 127, "%s: %s", r.title, r.choices[choice_index(r)].name);
    return buf + row * 128;
}

uint32_t fixed_label(const char* text) {
    uint32_t p = guest_alloc((uint32_t)std::strlen(text) + 1);
    std::strcpy(gptr(p), text);
    return p;
}

// Calls a lifted function the way the game would: return address, then stack arguments.
void call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> stack_args = {}) {
    c->esp -= 4 * ((uint32_t)stack_args.size() + 1);
    W32(c->esp, 0);
    uint32_t at = c->esp + 4;
    for (uint32_t a : stack_args) { W32(at, a); at += 4; }
    lookup(fn)(c);
}

// The game centres an item's text when it creates the item; do the same after changing it.
void set_item_text(Cpu* c, uint32_t item, uint32_t label) {
    std::strncpy(gptr(item + kItemLabel), gptr(label), 0x7f);
    uint32_t font = R32(kFontGlobal);
    if (!font) return;
    font += 0x14;
    const Cpu saved = *c;
    c->ecx = font;
    call(c, 0x4daf0, {0x3fc00000});             // menu text is 1.5 times the font's size
    c->ecx = font;
    c->edx = label;
    call(c, 0x4d9d0);                           // width of the text
    const int32_t width = (int32_t)c->eax;
    c->ecx = font;
    call(c, 0x4daf0, {0x3f800000});
    *c = saved;
    W16(item + kItemTextX, (uint16_t)(int16_t)((400 - width) / 2));
}

void apply(const Row& r, int value) {
    setting_set(r.key, value);
#ifdef _WIN32
    if (!std::strcmp(r.key, "fullscreen")) gpu::window_set_fullscreen(value != 0);
#endif
}

void quit() {
    hle_log("[menu] exit game");
    std::fflush(g_log);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#elif defined(__ANDROID__)
    gpu::android_finish();          // a process that just stops is restarted by Android
#else
    std::_Exit(0);
#endif
}

// ---- wrap-around ----
// The menu keeps its items in a list: next at +0, previous at +4, with an empty
// node at each end. Items with bit 1 of +0x38 set can't be selected.
const uint32_t kMenuUpdate = 0x579d0;           // the menu's every-frame function (menu in ecx)
const uint32_t kMenuBusy = 0x70, kMenuBusy2 = 0x74, kMenuRepeat = 0x7c, kMenuPlayer = 0x90;
const uint32_t kMenuSticks = 0x50;              // per player (12 bytes apart): the up/down input, value at +0x14
const uint32_t kMainCursor = 0x94;              // menu: selected row of the main menu
const uint32_t kRepeatDelay = 0x3df5c28f;       // 0.12 s, set whenever up or down is acted on
const uint32_t kStickThreshold = 0x1b2308;
GuestFn g_menu_update = nullptr;

uint32_t next_item(uint32_t item) {
    const uint32_t n = R32(item);
    return n && R32(n) ? n : 0;
}
uint32_t prev_item(uint32_t item) {
    const uint32_t p = R32(item + 4);
    return p && R32(p + 4) ? p : 0;
}
bool selectable(uint32_t item) { return !(R8(item + 0x38) & 2); }
float guest_float(uint32_t addr) { float v; uint32_t u = R32(addr); std::memcpy(&v, &u, 4); return v; }

void menu_update(Cpu* c) {
    const uint32_t menu = c->ecx, item = R32(menu + kMenuItem), repeat = R32(menu + kMenuRepeat);
    g_menu_update(c);
    // Up or down was acted on this frame, and the highlight had nowhere to go.
    if (!item || R32(menu + kMenuItem) != item || repeat == kRepeatDelay || R32(menu + kMenuRepeat) != kRepeatDelay) return;
    if (R32(menu + kMenuBusy) || R32(menu + kMenuBusy2) || (R8(menu + 0x38) & 2)) return;
    float push = 0;
    for (uint32_t player = 0; player < 2 && push == 0; player++) {
        const uint32_t who = R32(menu + kMenuPlayer);
        if (who != player && who != 0xffffffff) continue;
        const uint32_t input = R32(menu + kMenuSticks + 12 * player);
        if (!input) continue;
        const float v = guest_float(input + 0x14);
        if (std::fabs(v) > guest_float(kStickThreshold)) push = v;
    }
    if (push == 0) return;
    const bool down = push > 0;
    if (down ? next_item(item) : prev_item(item)) {         // only from the very end of the list
        for (uint32_t i = down ? next_item(item) : prev_item(item); i; i = down ? next_item(i) : prev_item(i))
            if (selectable(i)) return;
    }
    uint32_t first = item, target = 0;
    for (int guard = 0; prev_item(first) && guard < 64; guard++) first = prev_item(first);
    int rows = 0, target_row = 0;
    for (uint32_t i = first; i && rows < 64; i = next_item(i)) {
        if (!selectable(i)) continue;
        if (down ? !target : true) { target = i; target_row = rows; }      // down: the first; up: the last
        rows++;
    }
    if (!target || target == item) return;
    const uint32_t page = R32(menu + kMenuPage);
    if (page == kMainMenuPage) W32(menu + kMainCursor, (uint32_t)target_row);
    if (page == kOptionsPage) W32(menu + kOptionsCursor, (uint32_t)target_row);
    if (log_enabled("menu")) hle_log("[menu] wrapped %s to row %d (page %u)", down ? "down" : "up", target_row, page);
    const Cpu saved = *c;
    c->ecx = 0x43; c->edx = 0x148;
    call(c, 0xd57d0);                           // the menu's "moved" sound
    c->eax = 0; c->ecx = item;
    call(c, 0x57240, {menu});                   // un-highlight
    c->eax = 1; c->ecx = target;
    call(c, 0x57240, {menu});                   // highlight and select
    *c = saved;
}

}  // namespace

void menu_install() {
    g_menu_update = lookup(kMenuUpdate);
    if (g_menu_update) register_fn(kMenuUpdate, menu_update);
}

// Adds one menu item: previous item, row and text on the stack (popped by the
// callee), the menu in ebx; returns the item. Rows are 64 pixels apart, and a
// fifth row still fits on the screen.
void wrap_00057330(Cpu* c) {
    static const GuestFn add_item = lookup(0x57330);
    static const uint32_t exit_label = fixed_label(kExitText), settings_label = fixed_label(kSettingsText);
    const uint32_t row = R32(c->esp + 8), label = R32(c->esp + 12), menu = c->ebx;
    auto add = [&](uint32_t r, uint32_t text) {
        c->esp -= 16;
        W32(c->esp, 0);                 // return address, unused by a direct call
        W32(c->esp + 4, c->eax);        // the item before it
        W32(c->esp + 8, r);
        W32(c->esp + 12, text);
        add_item(c);
    };
    if (row == 0 && label != kDownloadLabel) g_page = Page::None;       // some other page is being built
    const bool settings = g_page == Page::Opening && menu == g_menu && row < 4 &&
                          (row != 0 || label == kDownloadLabel);
    if (settings) {                     // the Options page's four rows become the first four settings
        if (row == 0) W32(menu + kOptionsCursor, 0);        // start on the first row (the page may have fewer rows)
        W32(c->esp + 12, row_label((int)row));
        add_item(c);
        g_items[row] = c->eax;
        if (row == 3) {
            for (int r = 4; r < kRowCount; r++) {
                add((uint32_t)r, row_label(r));
                g_items[r] = c->eax;
            }
            g_page = Page::Shown;
        }
        return;
    }
    add_item(c);
    if (row == 3 && label == kOptionsLabel) add(4, exit_label);
    if (row == 3 && label == kPlayerOptionsLabel) add(4, settings_label);
}

// Opens a menu page (menu in eax, page on the stack). Any main menu entry the
// original doesn't recognise opens the Options page, "Exit Game" included.
// Leaving the settings page (B asks for the main menu) returns to Options.
void wrap_00057440(Cpu* c) {
    static const GuestFn open_page = lookup(0x57440);
    const uint32_t menu = c->eax, page = R32(c->esp + 4);
    if (page == kOptionsPage && R32(menu + kMenuPage) == kMainMenuPage) {
        const uint32_t item = R32(menu + kMenuItem);
        if (item && !std::strcmp(gptr(item + kItemLabel), kExitText)) quit();
    }
    if (g_page == Page::Shown && menu == g_menu) {
        g_page = Page::None;
        W32(c->esp + 4, kOptionsPage);
        W32(menu + kOptionsCursor, 4);  // back on "Display Settings"
    }
    open_page(c);
}

// The menu's "A pressed" sound (player in ecx): the one place every selection passes.
void wrap_000441a0(Cpu* c) {
    static const GuestFn select_sound = lookup(0x441a0);
    const bool from_menu = R32(c->esp) == kSelectReturn;
    const uint32_t menu = c->esi;
    select_sound(c);
    if (!from_menu || R32(menu + kMenuPage) != kOptionsPage) return;
    const uint32_t item = R32(menu + kMenuItem);
    if (!item) return;
    if (g_page == Page::Shown && menu == g_menu) {
        for (int r = 0; r < kRowCount; r++) {
            if (g_items[r] != item) continue;
            const Row& row = kRows[r];
            apply(row, row.choices[(choice_index(row) + 1) % row.count].value);
            set_item_text(c, item, row_label(r));
            return;
        }
        return;
    }
    if (g_page == Page::None && !std::strcmp(gptr(item + kItemLabel), kSettingsText)) {
        g_page = Page::Opening;         // the page is built once the current one has slid away
        g_menu = menu;
        const Cpu saved = *c;
        c->eax = menu;
        call(c, 0x57440, {kOptionsPage});
        *c = saved;
    }
}

}  // namespace tje
