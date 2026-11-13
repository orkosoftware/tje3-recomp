// Platform window (window_win32.cpp; Android will get its own).
#pragma once
#include <cstdint>
#include <string>

namespace tje::gpu {

struct NativeWindow {
    void* instance = nullptr;   // HINSTANCE
    void* hwnd = nullptr;       // HWND
};

bool window_create(int width, int height, bool fullscreen, const char* title, NativeWindow* out);
void window_set_title(const std::string& t);
void window_client_size(int* w, int* h);
bool window_take_resized();
void window_set_fullscreen(bool on);    // Windows; elsewhere the app always fills the screen

// Keyboard as a gamepad (until real controller input).
enum : uint32_t {
    KEY_UP = 1, KEY_DOWN = 2, KEY_LEFT = 4, KEY_RIGHT = 8, KEY_START = 16, KEY_BACK = 32,
    KEY_A = 1 << 8, KEY_B = 1 << 9, KEY_X = 1 << 10, KEY_Y = 1 << 11, KEY_BLACK = 1 << 12, KEY_WHITE = 1 << 13,
    KEY_LT = 1 << 14, KEY_RT = 1 << 15,
};
uint32_t window_keys();

#ifdef __ANDROID__
struct HandheldPad { float lx, ly, rx, ry, lt, rt; };      // sticks -1..1 (y down), triggers 0..1
HandheldPad window_pad();
bool window_foreground();
void window_wait_foreground();          // returns once the player is back in the app
void android_surface_lost();            // vulkan.cpp
void android_surface_ready(void* window);
void android_show_progress(float fraction);
#endif

}  // namespace tje::gpu
