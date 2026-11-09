// tje3: boots the recompiled ToeJam & Earl III.
//
//   tje3 [--data <dir containing default.xbe>] [--user <save dir>] [--dlc <dir>] [--log <file>]
//        [--headless] [--scale <n>] [--vsync] [--window <w>x<h>]
//
// The log goes to <user dir>/tje3.log unless --log names another file.
//
// Loads default.xbe into guest memory (headers at 0x10000, every section at its
// virtual address), installs the kernel HLE into the kernel thunk table,
// registers lifted and HLE functions, and runs the entry point.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gpu/gpu.h"
#include "settings.h"
#include "kobj.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

namespace tje {
int kernel_install(uint32_t thunk_table);
void kernel_tick();
void kernel_set_paths(const std::string& data, const std::string& user);
void register_hle_libs();
void install_gameplay_options();
void kernel_set_dlc(const std::string& dir);
std::string android_files_dir();
void trace_init();
void start_watchdog();
void start_preemption_timer();
void start_profiler();
void report_threads(const char* why, bool backtraces);
void crash_capture_init(const std::string& dump_path);
extern std::atomic<uint32_t> g_swaps;
}

using namespace tje;

static std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> d;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return d;
    std::fseek(f, 0, SEEK_END);
    d.resize((size_t)std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    std::fclose(f);
    return d;
}

// Started by double-clicking, the working directory is the exe's own folder
// (build/game); the default paths are relative to the project folder, so move
// there: the first folder at or above the exe that holds the game files.
static void enter_project_dir(const std::string& data) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(data + "/default.xbe", ec)) return;
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    for (fs::path dir = fs::path(exe).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / data / "default.xbe", ec)) { fs::current_path(dir, ec); return; }
        if (dir == dir.parent_path()) break;
    }
#endif
}

// The folder holding the exe, where the optional DLC folder lives.
static std::string exe_dir() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) return fs::path(exe).parent_path().generic_string();
#endif
#ifdef __ANDROID__
    return android_files_dir();                  // the app's own folder in shared storage
#endif
    std::error_code ec;
    return fs::current_path(ec).generic_string();
}

#ifdef __ANDROID__
int tje_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    std::string data = "original/xiso_extracted", user = "userdata", log_path, dlc;
    bool headless = false, windowed = false, user_given = false;
    // An installed copy keeps everything beside the exe: data/ (the player's disc
    // files) and userdata/. A development build uses the project folder instead.
    {
        std::error_code ec;
#ifdef __ANDROID__
        if (true) {                              // always: the game may still be packed inside the app
#else
        if (std::filesystem::exists(exe_dir() + "/data/default.xbe", ec)) {
#endif
            data = exe_dir() + "/data";
            user = exe_dir() + "/userdata";
        } else {
            enter_project_dir(data);
        }
    }
    gpu::Options gopt;
    for (int i = 1; i < argc; i++) {
        bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--headless")) headless = true;
        else if (!std::strcmp(argv[i], "--vsync")) gopt.vsync = true;
        else if (!more) break;
        else if (!std::strcmp(argv[i], "--data")) data = argv[++i];
        else if (!std::strcmp(argv[i], "--user")) { user = argv[++i]; user_given = true; }
        else if (!std::strcmp(argv[i], "--log")) log_path = argv[++i];
        else if (!std::strcmp(argv[i], "--dlc")) dlc = argv[++i];
        else if (!std::strcmp(argv[i], "--scale")) gopt.scale = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--window")) { windowed = true; std::sscanf(argv[++i], "%dx%d", &gopt.window_width, &gopt.window_height); }
    }
    // Without --log the log goes to <user dir>/tje3.log, so a crash leaves a record.
    if (log_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(user, ec);
        log_path = user + "/tje3.log";
    }
    g_log = std::fopen(log_path.c_str(), "w");
    if (!g_log) g_log = stderr;
#ifdef _WIN32
    crash_capture_init(user + "/tje3.dmp");
#endif
    if (std::getenv("TJE_TEST_CRASH")) {     // exercise the crash capture
        std::thread([] { volatile int* p = nullptr; *p = 1; }).join();
    }
    std::signal(SIGABRT, [](int) {
        hle_log("[crash] abort");
        std::fflush(g_log);
    });

#ifdef __ANDROID__
    gopt.scale = 1;                              // the Xbox's own resolution until a handheld proves it can do more
    gopt.vsync = true;                           // show each frame on a screen refresh
    gpu::init(gopt);                             // first, so unpacking can show its progress
    if (!gpu::android_unpack_game_files()) return 1;
#endif
    std::vector<uint8_t> xbe = read_file(data + "/default.xbe");
    if (xbe.size() < 0x1000 || std::memcmp(xbe.data(), "XBEH", 4) != 0) {
        std::fprintf(stderr, "cannot read %s/default.xbe\n", data.c_str());
        hle_log("cannot read %s/default.xbe", data.c_str());
#ifdef _WIN32
        if (!headless) {
            std::string msg = "Cannot read " + data + "/default.xbe\n(working folder: " +
                              std::filesystem::current_path().string() + ")";
            MessageBoxA(nullptr, msg.c_str(), "tje3", MB_ICONERROR);
        }
#endif
        return 1;
    }
    auto u32 = [&](uint32_t o) { uint32_t v; std::memcpy(&v, &xbe[o], 4); return v; };
    uint32_t base = u32(0x104), header_size = u32(0x108);
    uint32_t entry = u32(0x128) ^ 0xA8FC57ABu;            // retail XOR keys
    uint32_t thunks = u32(0x158) ^ 0x5B6D40B6u;

#ifdef _WIN32
    // 1 ms sleeps: the game sleeps in short slices and paces itself on vblank.
    // Windows 11 ignores timeBeginPeriod for throttled (e.g. window-less or
    // occluded) processes unless they opt out.
    PROCESS_POWER_THROTTLING_STATE throttle{};
    throttle.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttle.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttle.StateMask = 0;
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    if (auto set_info = (SetProcessInformationFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation"))
        set_info(GetCurrentProcess(), ProcessPowerThrottling, &throttle, sizeof throttle);
    timeBeginPeriod(1);
#endif
    trace_init();
    mem_init();
    std::memcpy(g_mem + base, xbe.data(), header_size);
    uint32_t nsec = u32(0x11c), sec_hdr = u32(0x120) - base;
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t h = sec_hdr + i * 0x38;
        uint32_t va = u32(h + 4), vsize = u32(h + 8), raw = u32(h + 12), rsize = u32(h + 16);
        std::memset(g_mem + va, 0, vsize);
        std::memcpy(g_mem + va, &xbe[raw], rsize < vsize ? rsize : vsize);
    }
    kernel_set_paths(data, user);
    kernel_set_dlc(dlc.empty() ? exe_dir() + "/DLC" : dlc);
    // Options live beside the exe, except with --user (a self-contained profile).
    // An ini from before the move is copied over once, and left where it was.
    std::string ini_dir = user_given ? user : exe_dir();
    if (!user_given) {
        std::error_code ec;
        std::filesystem::copy_file(user + "/tje3.ini", ini_dir + "/tje3.ini",
                                   std::filesystem::copy_options::skip_existing, ec);
    }
    settings_load(ini_dir);
    gopt.fullscreen = !windowed && setting_bool("fullscreen");

    for (unsigned i = 0; i < kLiftedCount; i++) register_fn(kLifted[i].addr, kLifted[i].fn);
    register_hle_libs();
    install_gameplay_options();
    int missing = kernel_install(thunks);
    hle_log("[boot] %u lifted functions, entry %08x, kernel thunks %08x (%d imports without HLE)",
            kLiftedCount, entry, thunks, missing);

    std::atomic<bool> running{true};
    std::thread ticker([&] {
        while (running) { kernel_tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    });
    ticker.detach();

#ifndef __ANDROID__
    if (!headless) gpu::init(gopt);
#endif
    // TJE_PEEK=<seconds>:<hex addr>:<hex len>[,...]: hex dump of guest memory at those times;
    // w<seconds>:<addr>:<len> instead watches the range for writes (first write per page).
    if (const char* pk = std::getenv("TJE_PEEK")) {
        std::string spec = pk;
        std::thread([spec] {
            auto t0 = std::chrono::steady_clock::now();
            size_t p = 0;
            while (p < spec.size()) {
                size_t q = spec.find(',', p);
                if (q == std::string::npos) q = spec.size();
                double secs = 0;
                unsigned addr = 0, len = 0;
                char kind = 0;
                std::string item = spec.substr(p, q - p);
                if (!item.empty() && (item[0] == 'w' || item[0] == 'd')) { kind = item[0]; item = item.substr(1); }
                std::sscanf(item.c_str(), "%lf:%x:%x", &secs, &addr, &len);
                std::this_thread::sleep_until(t0 + std::chrono::milliseconds((long long)(secs * 1000)));
                if (kind == 'd') {                   // d<seconds>:<addr>:0: report every write to that dword
                    mem_watch_dword(addr);
                    hle_log("[peek] watching dword %08x", addr);
                    p = q + 1;
                    continue;
                }
                if (kind == 'w') {                   // w<seconds>:<addr>:<len>: report the first write per page
                    mem_watch_writes(addr, len);
                    hle_log("[peek] watching %08x+%x", addr, len);
                    p = q + 1;
                    continue;
                }
                for (unsigned o = 0; o < len; o += 16) {
                    char line[128];
                    int n = std::snprintf(line, sizeof line, "%08x:", addr + o);
                    for (unsigned k = 0; k < 16 && o + k < len; k += 4) n += std::snprintf(line + n, sizeof line - n, " %08x", R32(addr + o + k));
                    hle_log("[peek] %s", line);
                }
                p = q + 1;
            }
        }).detach();
    }
    // Stall detector: once frames have started, a 5 s gap without a Swap logs
    // every thread (state and guest backtrace), then again every 30 s.
    std::thread([] {
        uint32_t last = 0;
        int quiet = 0, next_report = 5;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint32_t now = g_swaps.load();
            if (now != last || now == 0) { last = now; quiet = 0; next_report = 5; continue; }
            if (++quiet == next_report) {
                hle_log("[stall] no frame for %d s (after frame %u)", quiet, now);
                report_threads("stall", true);
                std::fflush(g_log);
                next_report += 30;
            }
        }
    }).detach();
    start_watchdog();
    start_preemption_timer();
    start_profiler();
    run_boot_thread(entry);
    wait_for_all_threads();
    hle_log("[boot] all threads finished");
    running = false;
    return 0;
}
