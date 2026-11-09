// The Android app: a NativeActivity with no Java of its own. android_main pumps
// the system's events; the game runs on a thread of its own with a large stack.
#ifdef __ANDROID__
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <android/asset_manager.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "window.h"

int tje_main(int argc, char** argv);

namespace tje {
void hle_log(const char* fmt, ...);
std::string android_files_dir();

namespace {
std::string g_files;
}
std::string android_files_dir() { return g_files; }
}  // namespace tje

namespace tje::gpu {

namespace {

std::mutex g_m;
std::condition_variable g_cv;
ANativeWindow* g_window = nullptr;
bool g_resumed = false;
AAssetManager* g_assets = nullptr;
std::atomic<bool> g_resized{false};
std::atomic<uint32_t> g_keys{0}, g_hat{0};
std::atomic<float> g_lx{0}, g_ly{0}, g_rx{0}, g_ry{0}, g_lt{0}, g_rt{0};

uint32_t key_bit(int32_t code) {
    switch (code) {
    case AKEYCODE_DPAD_UP: return KEY_UP;
    case AKEYCODE_DPAD_DOWN: return KEY_DOWN;
    case AKEYCODE_DPAD_LEFT: return KEY_LEFT;
    case AKEYCODE_DPAD_RIGHT: return KEY_RIGHT;
    case AKEYCODE_BUTTON_START: return KEY_START;
    case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return KEY_BACK;
    case AKEYCODE_BUTTON_THUMBL: return 64;
    case AKEYCODE_BUTTON_THUMBR: return 128;
    case AKEYCODE_BUTTON_A: return KEY_A;
    case AKEYCODE_BUTTON_B: return KEY_B;
    case AKEYCODE_BUTTON_X: return KEY_X;
    case AKEYCODE_BUTTON_Y: return KEY_Y;
    case AKEYCODE_BUTTON_L1: return KEY_WHITE;
    case AKEYCODE_BUTTON_R1: return KEY_BLACK;
    case AKEYCODE_BUTTON_L2: return KEY_LT;
    case AKEYCODE_BUTTON_R2: return KEY_RT;
    default: return 0;
    }
}

int32_t on_input(android_app*, AInputEvent* e) {
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        uint32_t bit = key_bit(AKeyEvent_getKeyCode(e));
        if (!bit) return 0;                 // volume and so on stay with the system
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) g_keys |= bit;
        else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_keys &= ~bit;
        return 1;
    }
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION && (AInputEvent_getSource(e) & AINPUT_SOURCE_JOYSTICK)) {
        auto axis = [&](int32_t a) { return AMotionEvent_getAxisValue(e, a, 0); };
        g_lx = axis(AMOTION_EVENT_AXIS_X);
        g_ly = axis(AMOTION_EVENT_AXIS_Y);
        g_rx = axis(AMOTION_EVENT_AXIS_Z);
        g_ry = axis(AMOTION_EVENT_AXIS_RZ);
        g_lt = std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE));
        g_rt = std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS));
        float hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
        g_hat = (hy < -0.5f ? KEY_UP : 0) | (hy > 0.5f ? KEY_DOWN : 0) | (hx < -0.5f ? KEY_LEFT : 0) | (hx > 0.5f ? KEY_RIGHT : 0);
        return 1;
    }
    return 0;
}

void set_resumed(bool resumed) {
    std::lock_guard<std::mutex> lk(g_m);
    g_resumed = resumed;
    g_cv.notify_all();
}

void on_command(android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: {
        android_surface_ready(app->window);
        std::lock_guard<std::mutex> lk(g_m);
        g_window = app->window;
        g_resized = true;
        g_cv.notify_all();
        break;
    }
    case APP_CMD_WINDOW_RESIZED: case APP_CMD_CONFIG_CHANGED:
        g_resized = true;
        break;
    case APP_CMD_TERM_WINDOW: {
        {
            std::lock_guard<std::mutex> lk(g_m);
            g_window = nullptr;
        }
        android_surface_lost();         // must finish before this handler returns
        break;
    }
    case APP_CMD_RESUME:
        hle_log("[window] back in the app");
        set_resumed(true);
        break;
    case APP_CMD_PAUSE:
        hle_log("[window] paused");
        g_keys = 0;
        set_resumed(false);
        break;
    case APP_CMD_DESTROY:
        hle_log("[window] closed by the system");
        _exit(0);
    case APP_CMD_LOST_FOCUS:
        g_keys = 0;
        break;
    }
}

// Hides the status and navigation bars. Must run on the system's UI thread.
void go_immersive(ANativeActivity* activity) {
    JNIEnv* env = activity->env;
    jclass cls = env->GetObjectClass(activity->clazz);
    jobject window = env->CallObjectMethod(activity->clazz, env->GetMethodID(cls, "getWindow", "()Landroid/view/Window;"));
    jclass wcls = env->GetObjectClass(window);
    jobject decor = env->CallObjectMethod(window, env->GetMethodID(wcls, "getDecorView", "()Landroid/view/View;"));
    jclass vcls = env->GetObjectClass(decor);
    // IMMERSIVE_STICKY | FULLSCREEN | HIDE_NAVIGATION | LAYOUT_* | LAYOUT_STABLE
    env->CallVoidMethod(decor, env->GetMethodID(vcls, "setSystemUiVisibility", "(I)V"), 0x1000 | 4 | 2 | 0x400 | 0x200 | 0x100);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void (*g_glue_focus)(ANativeActivity*, int);

void on_focus(ANativeActivity* activity, int focused) {
    if (focused) go_immersive(activity);
    g_glue_focus(activity, focused);
}

// Everything in TJE_* debugging comes from the environment; files/env.txt sets it here.
void load_env() {
    FILE* f = std::fopen((tje::android_files_dir() + "/env.txt").c_str(), "r");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        if (char* eq = std::strchr(line, '=')) { *eq = 0; setenv(line, eq + 1, 1); }
    }
    std::fclose(f);
}

}  // namespace

bool window_create(int, int, bool, const char*, NativeWindow* out) {
    std::unique_lock<std::mutex> lk(g_m);
    g_cv.wait(lk, [] { return g_window != nullptr; });
    out->instance = nullptr;
    out->hwnd = g_window;
    return true;
}

void window_set_title(const std::string&) {}

void window_client_size(int* w, int* h) {
    std::lock_guard<std::mutex> lk(g_m);
    *w = g_window ? ANativeWindow_getWidth(g_window) : 0;
    *h = g_window ? ANativeWindow_getHeight(g_window) : 0;
}

bool window_take_resized() { return g_resized.exchange(false); }
uint32_t window_keys() { return g_keys.load() | g_hat.load(); }
HandheldPad window_pad() { return {g_lx, g_ly, g_rx, g_ry, g_lt, g_rt}; }

bool window_foreground() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_resumed && g_window;
}

void window_wait_foreground() {
    std::unique_lock<std::mutex> lk(g_m);
    g_cv.wait(lk, [] { return g_resumed && g_window; });
}

// An APK built by setup carries the game's files. They are unpacked once into
// the app's own folder, where the kernel layer reads them like any other files.
bool android_unpack_game_files() {
    AAsset* list = AAssetManager_open(g_assets, "game_files.txt", AASSET_MODE_BUFFER);
    if (!list) return true;                     // a development build: the files were copied over by hand
    const std::string text((const char*)AAsset_getBuffer(list), (size_t)AAsset_getLength(list));
    AAsset_close(list);
    const std::string root = android_files_dir(), marker = root + "/unpacked.txt";
    {
        std::ifstream in(marker, std::ios::binary);
        std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (have == text) return true;
    }
    struct Item { uint64_t size; std::string path; };
    std::vector<Item> items;
    uint64_t total = 0, done = 0;
    for (size_t at = 0; at < text.size();) {
        size_t tab = text.find('\t', at), end = text.find('\n', at);
        if (tab == std::string::npos || end == std::string::npos) break;
        items.push_back({std::strtoull(text.c_str() + at, nullptr, 10), text.substr(tab + 1, end - tab - 1)});
        total += items.back().size;
        at = end + 1;
    }
    hle_log("[unpack] %zu files, %llu MB", items.size(), (unsigned long long)(total >> 20));
    std::vector<char> buf(1 << 20);
    auto shown = std::chrono::steady_clock::now();
    auto show = [&](bool force) {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - shown < std::chrono::milliseconds(200)) return;
        shown = now;
        android_show_progress(total ? (float)((double)done / total) : 1.0f);
    };
    show(true);
    namespace fs = std::filesystem;
    for (const Item& item : items) {
        const std::string target = root + "/" + item.path, part = target + ".part";
        std::error_code ec;
        fs::create_directories(fs::path(target).parent_path(), ec);
        if (fs::exists(target, ec) && fs::file_size(target, ec) == item.size) {   // from an earlier, interrupted run
            done += item.size;
            show(false);
            continue;
        }
        AAsset* a = AAssetManager_open(g_assets, ("game/" + item.path).c_str(), AASSET_MODE_STREAMING);
        FILE* out = a ? std::fopen(part.c_str(), "wb") : nullptr;
        if (!out) {
            hle_log("[unpack] cannot write %s", item.path.c_str());
            if (a) AAsset_close(a);
            return false;
        }
        int n;
        bool ok = true;
        while (ok && (n = AAsset_read(a, buf.data(), buf.size())) > 0) {
            ok = std::fwrite(buf.data(), 1, (size_t)n, out) == (size_t)n;
            done += (uint64_t)n;
            show(false);
        }
        std::fclose(out);
        AAsset_close(a);
        if (!ok || n < 0) {
            hle_log("[unpack] failed on %s (is the device full?)", item.path.c_str());
            fs::remove(part, ec);
            return false;
        }
        fs::rename(part, target, ec);
    }
    show(true);
    if (FILE* f = std::fopen(marker.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    hle_log("[unpack] done");
    return true;
}

void android_start(android_app* app) {
    g_files = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
    g_assets = app->activity->assetManager;
    load_env();
    app->onAppCmd = on_command;
    app->onInputEvent = on_input;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 20);
    pthread_t game;
    pthread_create(&game, &attr, [](void*) -> void* {
        char name[] = "tje3";
        char* argv[] = {name, nullptr};
        _exit(tje_main(1, argv));
    }, nullptr);
    pthread_attr_destroy(&attr);
    for (;;) {
        int events;
        android_poll_source* source;
        if (ALooper_pollOnce(-1, nullptr, &events, (void**)&source) >= 0 && source) source->process(app, source);
        if (app->destroyRequested) _exit(0);
    }
}

}  // namespace tje::gpu

void android_main(android_app* app) { tje::gpu::android_start(app); }

// The entry the manifest names: the standard one, plus hiding the system bars.
extern "C" JNIEXPORT void tje_activity_create(ANativeActivity* activity, void* saved, size_t size) {
    ANativeActivity_onCreate(activity, saved, size);
    tje::gpu::g_glue_focus = activity->callbacks->onWindowFocusChanged;
    activity->callbacks->onWindowFocusChanged = tje::gpu::on_focus;
    ANativeActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    tje::gpu::go_immersive(activity);
}
#endif
