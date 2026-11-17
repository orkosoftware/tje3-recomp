// HLE of the Xbox input API (XPP: XInitDevices, XGetDevices, XInput*).
//
// Up to four gamepads. Port 0 always has one; the others appear as controllers are
// connected. State comes from input_poll(), which also replays TJE_INPUT, a script
// of timed presses for headless runs:
//   TJE_INPUT="3000:start,4500:a,6000:down,7000:a@2"   (milliseconds after boot : button[@player])
// Buttons: up down left right start back lthumb rthumb a b x y black white lt rt.
// With a window, the keyboard also drives the pad: arrows, Enter = START,
// Esc = BACK, Z/Space = A, X = B, A = X, S = Y, Q/W = triggers, C/V = white/black.
// Each PC controller (XInput) maps onto the Xbox pad: sticks,
// d-pad, A/B/X/Y, triggers; LB = WHITE, RB = BLACK; Start, Back, stick clicks.
// Options in tje3.ini can invert the camera's horizontal turn and look-mode pitch.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
uint32_t g_reported = 0;            // ports the game has been told about
uint32_t g_packet = 0;

struct Press { int64_t at_ms; std::string button; int64_t hold_ms; int port; };   // "ms:button[@player][:hold]"
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
            int port = 0;
            size_t at = button.find('@');
            if (at != std::string::npos) { port = std::atoi(button.substr(at + 1).c_str()) - 1; button = button.substr(0, at); }
            g_script.push_back({std::atoll(item.substr(0, colon).c_str()), button, hold, port});
        }
        p = q + 1;
    }
}

#ifdef _WIN32
// XInput, loaded at run time (xinput1_4.dll, else xinput9_1_0.dll).
struct XPad { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XState { DWORD packet; XPad pad; };
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);

XInputGetStateFn xinput_get() {
    static XInputGetStateFn get = [] {
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        return m ? (XInputGetStateFn)GetProcAddress(m, "XInputGetState") : nullptr;
    }();
    return get;
}

// Each PC controller keeps the Xbox port it was given until it is unplugged; a new
// one takes the lowest free port. g_slot[port] is its XInput index, or -1.
std::mutex g_pads_m;
int g_slot[4] = {-1, -1, -1, -1};

// Once a second: drop controllers that have gone and look for new ones
// (XInputGetState is slow on empty slots, so not on every poll).
void scan_pads() {
    static auto next_scan = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now < next_scan) return;
    next_scan = now + std::chrono::seconds(1);
    XInputGetStateFn get = xinput_get();
    if (!get) return;
    XState st{};
    bool used[4] = {};
    for (int& slot : g_slot) {
        if (slot < 0) continue;
        if (get((DWORD)slot, &st) == 0) used[slot] = true;
        else { hle_log("[input] controller %d disconnected", slot); slot = -1; }
    }
    for (int i = 0; i < 4; i++) {
        if (used[i] || get((DWORD)i, &st) != 0) continue;
        for (int port = 0; port < 4; port++)
            if (g_slot[port] < 0) {
                g_slot[port] = i;
                hle_log("[input] controller %d connected as player %d", i, port + 1);
                break;
            }
    }
}

uint32_t host_ports() {
    std::lock_guard<std::mutex> lk(g_pads_m);
    scan_pads();
    uint32_t mask = 0;
    for (int port = 0; port < 4; port++)
        if (g_slot[port] >= 0) mask |= 1u << port;
    return mask;
}

bool host_pad(int port, XPad* out) {
    std::lock_guard<std::mutex> lk(g_pads_m);
    scan_pads();
    XState st{};
    if (g_slot[port] < 0 || xinput_get()((DWORD)g_slot[port], &st) != 0) return false;
    *out = st.pad;
    return true;
}

// A PC controller onto the Xbox gamepad at `gamepad` (merged with what is already there).
void apply_host_pad(uint32_t gamepad, int port) {
    XPad p;
    if (!host_pad(port, &p)) return;
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
#elif defined(__ANDROID__)
uint32_t host_ports() { return gpu::window_ports(); }
#else
uint32_t host_ports() { return 0; }
#endif

// Ports the game sees a gamepad in. Port 0 always has one (keyboard, or the handheld's
// own controls); TJE_PADS=n pretends n are plugged in, for tests.
uint32_t ports_now() {
    static const uint32_t pretend = [] {
        const char* s = std::getenv("TJE_PADS");
        int n = s ? std::atoi(s) : 0;
        return n >= 4 ? 15u : n > 0 ? (1u << n) - 1 : 0u;
    }();
    return 1 | pretend | (host_ports() & 15);
}

// XINPUT_GAMEPAD { WORD wButtons; BYTE bAnalogButtons[8]; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; }
void input_poll(uint32_t gamepad, int port) {
    load_script();
    std::memset(gptr(gamepad), 0, 18);
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - g_start).count();
    static const char* digital[] = {"up", "down", "left", "right", "start", "back", "lthumb", "rthumb"};
    static const char* analog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    for (const Press& pr : g_script) {
        if (pr.port != port || now < pr.at_ms || now > pr.at_ms + pr.hold_ms) continue;
        for (int i = 0; i < 8; i++) {
            if (pr.button == digital[i]) W16(gamepad, R16(gamepad) | (1u << i));
            if (pr.button == analog[i]) W8(gamepad + 2 + i, 255);
        }
    }
#ifdef __ANDROID__
    uint32_t keys = gpu::window_port_keys(port);
#else
    uint32_t keys = port == 0 ? gpu::window_keys() : 0;     // the keyboard is player one
#endif
    W16(gamepad, R16(gamepad) | (keys & 0xff));
    for (int i = 0; i < 8; i++)
        if (keys & (0x100u << i)) W8(gamepad + 2 + i, 255);
#ifdef _WIN32
    apply_host_pad(gamepad, port);
#elif defined(__ANDROID__)
    {
        gpu::HandheldPad p = gpu::window_pad(port);
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
    g_reported = ports_now();
    if (log_enabled("input")) hle_log("[input] XGetDevices(%08x) -> %x", A(c, 0), g_reported);
    RET(c, g_reported, 1);
}
void hle_001ac97f(Cpu* c) {                                       // XGetDeviceChanges(type, pIns, pRem)
    uint32_t now = ports_now(), in = now & ~g_reported, out = g_reported & ~now;
    g_reported = now;
    if (A(c, 1)) W32(A(c, 1), in);
    if (A(c, 2)) W32(A(c, 2), out);
    if (in || out) hle_log("[input] gamepad ports: inserted %x, removed %x", in, out);
    RET(c, in || out ? 1 : 0, 3);
}
void hle_001ac5c2(Cpu* c) {                                       // XInputOpen(type, port, slot, pPolling)
    uint32_t port = A(c, 1);
    bool ok = port < 4 && (ports_now() >> port & 1);
    hle_log("[input] XInputOpen(%08x, port %u, slot %u) -> %s", A(c, 0), port, A(c, 2), ok ? "opened" : "nothing there");
    RET(c, ok ? kGamepadHandle + port : 0, 4);
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
    uint32_t st = A(c, 1), port = A(c, 0) - kGamepadHandle;
    if (port >= 4 || !(ports_now() >> port & 1)) { RET(c, 1167, 2); return; }   // ERROR_DEVICE_NOT_CONNECTED
    W32(st, ++g_packet);
    input_poll(st + 4, (int)port);
    if (log_enabled("input") && (R16(st + 4) || R8(st + 6) || R8(st + 7)))
        hle_log("[input] player %u buttons %04x a=%u b=%u", port + 1, R16(st + 4), R8(st + 6), R8(st + 7));
    RET(c, 0, 2);
}
void hle_001ac86e(Cpu* c) {                                       // XInputSetState(h, pFeedback)
    uint32_t fb = A(c, 1);
    if (fb) W32(fb, 0);                                           // header.dwStatus = ERROR_SUCCESS
    RET(c, 0, 2);
}

}  // namespace tje
