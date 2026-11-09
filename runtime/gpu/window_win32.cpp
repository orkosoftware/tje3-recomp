// The game window (Win32). It lives on its own host thread, which pumps its
// messages: the main host thread runs the boot guest thread and never returns
// to a message loop. Closing the window ends the process.
#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "window.h"

namespace tje {
extern FILE* g_log;
void hle_log(const char* fmt, ...);
void report_threads(const char* why, bool backtraces);
}

namespace tje::gpu {

namespace {

HWND g_hwnd = nullptr;
bool g_fullscreen = false;
WINDOWPLACEMENT g_placement{sizeof g_placement};   // of the window while fullscreen
std::atomic<bool> g_resized{false};
std::atomic<uint32_t> g_keys{0};

uint32_t key_bit(WPARAM vk) {
    switch (vk) {
    case VK_UP: return KEY_UP;
    case VK_DOWN: return KEY_DOWN;
    case VK_LEFT: return KEY_LEFT;
    case VK_RIGHT: return KEY_RIGHT;
    case VK_RETURN: return KEY_START;
    case VK_ESCAPE: case VK_BACK: return KEY_BACK;
    case 'Z': case VK_SPACE: return KEY_A;
    case 'X': return KEY_B;
    case 'A': return KEY_X;
    case 'S': return KEY_Y;
    case 'Q': return KEY_LT;
    case 'W': return KEY_RT;
    case 'C': return KEY_WHITE;
    case 'V': return KEY_BLACK;
    default: return 0;
    }
}

// Borderless on the monitor the window is on; going back restores the window
// exactly as it was (position, size, maximised).
void set_fullscreen(HWND h, bool on) {
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    MONITORINFO mi{sizeof mi};
    if (on) {
        if (!GetWindowPlacement(h, &g_placement) ||
            !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        SetWindowLongPtrW(h, GWL_STYLE, (style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW) | WS_POPUP);
        const RECT& m = mi.rcMonitor;
        SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, (style & ~(LONG_PTR)WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = on;
}

void log_mode(HWND h) {
    RECT c{};
    GetClientRect(h, &c);
    hle_log("[window] %s, %ld x %ld", g_fullscreen ? "fullscreen" : "windowed", c.right, c.bottom);
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        hle_log("[window] closed by the user");
        report_threads("close", true);
        std::fflush(g_log);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_SIZE:
        g_resized = true;
        return 0;
    case WM_SYSCHAR:                             // Alt+Enter would beep
        if (wp == '\r') return 0;
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp == VK_RETURN) {   // Alt+Enter; not the START button
            if (!(lp & (1 << 30))) {                     // not a key repeat
                set_fullscreen(h, !g_fullscreen);
                log_mode(h);
            }
            return 0;
        }
        g_keys |= key_bit(wp);
        if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        g_keys &= ~key_bit(wp);
        return 0;
    case WM_KILLFOCUS:
        g_keys = 0;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (g_fullscreen && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool window_create(int width, int height, bool fullscreen, const char* title, NativeWindow* out) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string t = title;
    std::thread([&, width, height, fullscreen, t] {
        // Real pixels: without this, Windows scales the picture on a high-DPI monitor.
        SetProcessDPIAware();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"tje3";
        RegisterClassW(&wc);
        RECT r{0, 0, width, height};
        DWORD style = WS_OVERLAPPEDWINDOW;
        AdjustWindowRect(&r, style, FALSE);
        std::wstring wt(t.begin(), t.end());
        HWND h = CreateWindowExW(0, wc.lpszClassName, wt.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
        if (h) {
            if (fullscreen) set_fullscreen(h, true);
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
            log_mode(h);
        }
        {
            std::lock_guard<std::mutex> lk(m);
            g_hwnd = h;
            done = true;
        }
        cv.notify_all();
        if (!h) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }).detach();
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
    if (!g_hwnd) return false;
    out->instance = GetModuleHandleW(nullptr);
    out->hwnd = g_hwnd;
    return true;
}

void window_set_title(const std::string& t) {
    if (g_hwnd) SetWindowTextA(g_hwnd, t.c_str());
}

void window_client_size(int* w, int* h) {
    RECT r{};
    if (g_hwnd) GetClientRect(g_hwnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

bool window_take_resized() { return g_resized.exchange(false); }
uint32_t window_keys() { return g_keys.load(); }

}  // namespace tje::gpu
#endif
