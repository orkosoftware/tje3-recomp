// HLE of the Xbox input API (XPP: XInitDevices, XGetDevices, XInput*).
//
// One gamepad is connected on port 0. Its state comes from input_poll(), which
// for now replays TJE_INPUT, a script of timed presses for headless runs:
//   TJE_INPUT="3000:start,4500:a,6000:down"   (milliseconds after boot : button)
// Buttons: up down left right start back lthumb rthumb a b x y black white lt rt.
// With a window, the keyboard also drives the pad: arrows, Enter = START,
// Esc = BACK, Z/Space = A, X = B, A = X, S = Y, Q/W = triggers, C/V = white/black.
// A PC controller (XInput, first one connected) maps onto the Xbox pad: sticks,
// d-pad, A/B/X/Y, triggers; LB = WHITE, RB = BLACK; Start, Back, stick clicks.
// Options in tje3.ini can invert the camera's horizontal turn and look-mode pitch.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/window.h"
#include "guest.h"
#include "settings.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace tje {

namespace {

constexpr uint32_t kGamepadHandle = 0x47504400;     // "GPD"
bool g_reported_insertion = false;
uint32_t g_packet = 0;

struct Press { int64_t at_ms; std::string button; int64_t hold_ms; };   // "ms:button" or "ms:button:hold"
std::vector<Press> g_script;
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

void load_script() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    const char* s = std::getenv("TJE_INPUT");
    if (!s) return;
    std::string spec = s;
    size_t p = 0;
    while (p < spec.size()) {
        size_t q = spec.find(',', p);
        if (q == std::string::npos) q = spec.size();
        std::string item = spec.substr(p, q - p);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
        {
            std::string button = item.substr(colon + 1);
            int64_t hold = 200;
            size_t c2 = button.find(':');
            if (c2 != std::string::npos) { hold = std::atoll(button.substr(c2 + 1).c_str()); button = button.substr(0, c2); }
            g_script.push_back({std::atoll(item.substr(0, colon).c_str()), button, hold});
        }
        p = q + 1;
    }
}

#ifdef _WIN32
// XInput, loaded at run time (xinput1_4.dll, else xinput9_1_0.dll).
struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);

bool host_pad(XPad* out) {
    static XInputGetStateFn get = [] {
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        return m ? (XInputGetStateFn)GetProcAddress(m, "XInputGetState") : nullptr;
    }();
    if (!get) return false;
    // Check unconnected slots only now and then (XInputGetState is slow on empty ones).
    static int slot = -1;
    static auto next_scan = std::chrono::steady_clock::now();
    XState st{};
    if (slot >= 0 && get((DWORD)slot, &st) == 0) { *out = st.pad; return true; }
    slot = -1;
    auto now = std::chrono::steady_clock::now();
    if (now < next_scan) return false;
    next_scan = now + std::chrono::seconds(1);
    for (DWORD i = 0; i < 4; i++)
        if (get(i, &st) == 0) {
            slot = (int)i;
            hle_log("[input] controller %lu connected", i);
            *out = st.pad;
            return true;
        }
    return false;
}

// A PC controller onto the Xbox gamepad at `gamepad` (merged with what is already there).
void apply_host_pad(uint32_t gamepad) {
    XPad p;
    if (!host_pad(&p)) return;
    // Low byte: d-pad, Start, Back, stick clicks have the same bits on both pads.
    W16(gamepad, R16(gamepad) | (p.buttons & 0xff));
    auto press = [&](int i, bool on) { if (on) W8(gamepad + 2 + i, 255); };
    press(0, p.buttons & 0x1000);                 // A
    press(1, p.buttons & 0x2000);                 // B
    press(2, p.buttons & 0x4000);                 // X
    press(3, p.buttons & 0x8000);                 // Y
    press(4, p.buttons & 0x0200);                 // BLACK <- RB
    press(5, p.buttons & 0x0100);                 // WHITE <- LB
    if (p.lt > R8(gamepad + 8)) W8(gamepad + 8, p.lt);
    if (p.rt > R8(gamepad + 9)) W8(gamepad + 9, p.rt);
    W16(gamepad + 10, (uint16_t)p.lx);
    W16(gamepad + 12, (uint16_t)p.ly);
    W16(gamepad + 14, (uint16_t)p.rx);
    W16(gamepad + 16, (uint16_t)p.ry);
}
#endif

// XINPUT_GAMEPAD { WORD wButtons; BYTE bAnalogButtons[8]; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
void input_poll(uint32_t gamepad) {
    load_script();
    std::memset(gptr(gamepad), 0, 18);
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - g_start).count();
    static const char* digital[] = {"up", "down", "left", "right", "start", "back", "lthumb", "rthumb"};
    static const char* analog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    for (const Press& pr : g_script) {
        if (now < pr.at_ms || now > pr.at_ms + pr.hold_ms) continue;
        for (int i = 0; i < 8; i++) {
            if (pr.button == digital[i]) W16(gamepad, R16(gamepad) | (1u << i));
            if (pr.button == analog[i]) W8(gamepad + 2 + i, 255);
        }
    }
    uint32_t keys = gpu::window_keys();
    W16(gamepad, R16(gamepad) | (keys & 0xff));
    for (int i = 0; i < 8; i++)
        if (keys & (0x100u << i)) W8(gamepad + 2 + i, 255);
#ifdef _WIN32
    apply_host_pad(gamepad);
#elif defined(__ANDROID__)
    {
        gpu::HandheldPad p = gpu::window_pad();
        auto stick = [](float v) {
            int i = (int)(v * 32767.0f);
            return (uint16_t)(int16_t)(i > 32767 ? 32767 : i < -32768 ? -32768 : i);
        };
        W16(gamepad + 10, stick(p.lx));
        W16(gamepad + 12, stick(-p.ly));                // up is positive on the Xbox
        W16(gamepad + 14, stick(p.rx));
        W16(gamepad + 16, stick(-p.ry));
        uint8_t lt = (uint8_t)(p.lt * 255.0f), rt = (uint8_t)(p.rt * 255.0f);
        if (lt > R8(gamepad + 8)) W8(gamepad + 8, lt);
        if (rt > R8(gamepad + 9)) W8(gamepad + 9, rt);
    }
#endif
    // Player options: camera turning and look-mode pitch the "natural" way.
    auto negate = [](uint32_t a) {
        int16_t v = (int16_t)R16(a);
        W16(a, (uint16_t)(int16_t)(v == -32768 ? 32767 : -v));
    };
    bool look = R8(gamepad + 8) > 30;                                       // left trigger held: look mode
    if (setting_bool("invert_camera_x") && !look) negate(gamepad + 14);     // right stick X (camera orbit)
    if (setting_bool("invert_look_y") && look) {
        negate(gamepad + 12);                                               // left stick Y
        negate(gamepad + 16);                                               // right stick Y
    }
}

}  // namespace

void hle_001ac958(Cpu* c) { RET(c, 0, 2); }                       // XInitDevices(count, types)
void hle_001ac95d(Cpu* c) {                                       // XGetDevices(type) -> port mask
    if (log_enabled("input")) hle_log("[input] XGetDevices(%08x)", A(c, 0));
    g_reported_insertion = true;
    RET(c, 1, 1);
}
void hle_001ac97f(Cpu* c) {                                       // XGetDeviceChanges(type, pIns, pRem)
    bool first = !g_reported_insertion;
    g_reported_insertion = true;
    if (A(c, 1)) W32(A(c, 1), first ? 1 : 0);
    if (A(c, 2)) W32(A(c, 2), 0);
    RET(c, first ? 1 : 0, 3);
}
void hle_001ac5c2(Cpu* c) {                                       // XInputOpen(type, port, slot, pPolling)
    if (log_enabled("input")) hle_log("[input] XInputOpen(%08x, port %u, slot %u)", A(c, 0), A(c, 1), A(c, 2));
    RET(c, A(c, 1) == 0 ? kGamepadHandle : 0, 4);
}
void hle_001ac618(Cpu* c) { RET(c, 0, 1); }                       // XInputClose(h)
void hle_001ac624(Cpu* c) {                                       // XInputGetCapabilities(h, pCaps)
    uint32_t caps = A(c, 1);
    std::memset(gptr(caps), 0, 0x16);
    W8(caps, 1);                                                  // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    W16(caps + 2, 0xff);
    for (int i = 0; i < 8; i++) W8(caps + 4 + i, 0xff);
    for (int i = 0; i < 4; i++) W16(caps + 12 + 2 * i, 0xffff);
    RET(c, 0, 2);
}
void hle_001ac802(Cpu* c) {                                       // XInputGetState(h, pState)
    uint32_t st = A(c, 1);
    if (A(c, 0) != kGamepadHandle) { RET(c, 1167, 2); return; }   // ERROR_DEVICE_NOT_CONNECTED
    W32(st, ++g_packet);
    input_poll(st + 4);
    if (log_enabled("input") && (R16(st + 4) || R8(st + 6) || R8(st + 7)))
        hle_log("[input] state buttons %04x a=%u b=%u", R16(st + 4), R8(st + 6), R8(st + 7));
    RET(c, 0, 2);
}
void hle_001ac86e(Cpu* c) {                                       // XInputSetState(h, pFeedback)
    uint32_t fb = A(c, 1);
    if (fb) W32(fb, 0);                                           // header.dwStatus = ERROR_SUCCESS
    RET(c, 0, 2);
}

}  // namespace tje
