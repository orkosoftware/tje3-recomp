// Adds "Exit Game" to the bottom of the main menu. The Xbox had no need for it:
// players pressed the power button.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../guest.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace tje {

extern FILE* g_log;

namespace {

const uint32_t kOptionsLabel = 0x1d04b8;        // "Options", the main menu's last entry
const uint32_t kMainMenuPage = 1, kOptionsPage = 5;
const uint32_t kItemLabel = 0x54;               // a menu item's copy of its text
const char kExitText[] = "Exit Game";

uint32_t exit_label() {
    static const uint32_t a = [] {
        uint32_t p = guest_alloc(sizeof kExitText);
        std::memcpy(gptr(p), kExitText, sizeof kExitText);
        return p;
    }();
    return a;
}

}  // namespace

// Adds one menu item: previous item, row and text on the stack (popped by the
// callee), the menu in ebx; returns the item. Rows are 64 pixels apart, and a
// fifth row still fits on the screen.
void wrap_00057330(Cpu* c) {
    static const GuestFn add_item = lookup(0x57330);
    const uint32_t row = R32(c->esp + 8), label = R32(c->esp + 12);
    add_item(c);
    if (row == 3 && label == kOptionsLabel) {
        c->esp -= 16;
        W32(c->esp, 0);                 // return address, unused by a direct call
        W32(c->esp + 4, c->eax);
        W32(c->esp + 8, 4);
        W32(c->esp + 12, exit_label());
        add_item(c);
    }
}

// Opens a menu page (menu in eax, page on the stack). Any main menu entry the
// original doesn't recognise opens the Options page, "Exit Game" included.
void wrap_00057440(Cpu* c) {
    static const GuestFn open_page = lookup(0x57440);
    const uint32_t menu = c->eax, page = R32(c->esp + 4);
    if (page == kOptionsPage && R32(menu + 0x80) == kMainMenuPage) {
        const uint32_t item = R32(menu + 0x40);
        if (item && !std::strcmp(gptr(item + kItemLabel), kExitText)) {
            hle_log("[menu] exit game");
            std::fflush(g_log);
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), 0);
#else
            std::_Exit(0);
#endif
        }
    }
    open_page(c);
}

}  // namespace tje
