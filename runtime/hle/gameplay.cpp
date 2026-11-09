// Optional changes to the game's rules, all off by default (see tje3.ini).
#include "../guest.h"
#include "../settings.h"

namespace tje {

namespace {

GuestFn g_boogeyman_message;

// Boogeyman message handler (this in ecx, message in edx, three stack arguments).
// In the original, message 0x72 fires 20-45 s after a boogeyman hides: it moves
// next to a player, loses its conversion and attacks. With the option on, one
// that was converted stays hidden; unconverted ones behave as usual.
void boogeyman_message(Cpu* c) {
    const uint32_t self = c->ecx;
    if (c->edx == 0x72 && (R32(self + 0x214) & 0x100) && setting_bool("boogeymen_stay_converted")) {
        W32(self + 0x4a0, 0xffffffff);      // the same timers the original handler stops
        W32(self + 0x34c, 0xffffffff);
        hle_log("[gameplay] converted boogeyman %08x stays gone", self);
        RET(c, 1, 3);
        return;
    }
    g_boogeyman_message(c);
}

}  // namespace

void install_gameplay_options() {
    g_boogeyman_message = lookup(0xc7940);
    register_fn(0xc7940, boogeyman_message);
}

}  // namespace tje
