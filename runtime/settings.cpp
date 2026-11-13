// Player options (see settings.h). Unknown keys are kept; missing keys get
// their defaults and are written back, so the file always lists every option.
#include "settings.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include "guest.h"

namespace tje {

namespace {

struct Option { const char* key; const char* value; const char* comment; };

#ifdef __ANDROID__
#define TJE_DEFAULT_RESOLUTION "1"
#define TJE_DEFAULT_VSYNC "1"
#else
#define TJE_DEFAULT_RESOLUTION "0"
#define TJE_DEFAULT_VSYNC "0"
#endif

const Option kOptions[] = {
    {"display_mode", "0", "picture shape: 0 = original 4:3 with black bars, 1 = widescreen (see more to the sides), 2 = stretch to fill, 3 = sharp (whole-number scaling)"},
    {"resolution", TJE_DEFAULT_RESOLUTION, "render resolution: 0 = match the screen, 1 to 4 = that many times the Xbox's 640x480"},
    {"smoothing", "1", "1: smooth the picture when it is scaled to the screen; 0: hard pixel edges"},
    {"vsync", TJE_DEFAULT_VSYNC, "1: show frames in step with the screen (no tearing); 0: show them as soon as they are ready"},
    {"fullscreen", "1", "1: start in fullscreen on the main monitor; 0: start in a window (--window <w>x<h> also starts in one)"},
    {"invert_camera_x", "1", "1: right stick right turns the camera right (the original turns it left); look mode is not affected"},
    {"invert_look_y", "1", "1: in look mode (left trigger held) up looks up (the original is flight-style)"},
    {"master_volume", "100", "sound volume, 0-100"},
    {"shadow_enhance", "0", "enhancement: where a character's shadow overlaps a scenery shadow, darken it by this percent (0 = original look), e.g. 35"},
    {"boogeymen_stay_converted", "0", "1: a boogeyman you convert stays gone instead of coming back to attack (0 = original game)"},
};

std::map<std::string, std::string> g_values;
std::mutex g_m;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string g_path;
std::filesystem::file_time_type g_mtime;
std::chrono::steady_clock::time_point g_next_check;

void read_file_locked() {
    if (FILE* f = std::fopen(g_path.c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = trim(line);
            if (l.empty() || l[0] == '#' || l[0] == ';') continue;
            size_t eq = l.find('=');
            if (eq != std::string::npos) g_values[trim(l.substr(0, eq))] = trim(l.substr(eq + 1));
        }
        std::fclose(f);
    }
    std::error_code ec;
    g_mtime = std::filesystem::last_write_time(g_path, ec);
}

// Edits made while the game runs take effect within a second.
void reload_if_changed_locked() {
    auto now = std::chrono::steady_clock::now();
    if (g_path.empty() || now < g_next_check) return;
    g_next_check = now + std::chrono::seconds(1);
    std::error_code ec;
    auto t = std::filesystem::last_write_time(g_path, ec);
    if (ec || t == g_mtime) return;
    read_file_locked();
    std::string summary;
    for (auto& [k, v] : g_values) summary += " " + k + "=" + v;
    hle_log("[settings] reloaded:%s", summary.c_str());
}

void write_file_locked() {
    if (FILE* f = std::fopen(g_path.c_str(), "w")) {
        std::fprintf(f, "# ToeJam & Earl III options\n");
        for (const Option& o : kOptions) std::fprintf(f, "\n# %s\n%s = %s\n", o.comment, o.key, g_values[o.key].c_str());
        for (auto& [k, v] : g_values) {
            bool known = false;
            for (const Option& o : kOptions) known |= k == o.key;
            if (!known) std::fprintf(f, "%s = %s\n", k.c_str(), v.c_str());
        }
        std::fclose(f);
    }
    std::error_code ec;
    g_mtime = std::filesystem::last_write_time(g_path, ec);
}

}  // namespace

void setting_set(const char* key, int value) {
    std::lock_guard<std::mutex> lk(g_m);
    g_values[key] = std::to_string(value);
    if (!g_path.empty()) write_file_locked();
    hle_log("[settings] %s = %d", key, value);
}

void settings_load(const std::string& user_dir) {
    std::lock_guard<std::mutex> lk(g_m);
    std::error_code ec;
    std::filesystem::create_directories(user_dir, ec);
    std::string path = user_dir + "/tje3.ini";
    g_path = path;
    read_file_locked();
    bool missing = false;
    for (const Option& o : kOptions)
        if (!g_values.count(o.key)) { g_values[o.key] = o.value; missing = true; }
    if (missing) write_file_locked();
    std::string summary;
    for (auto& [k, v] : g_values) summary += " " + k + "=" + v;
    hle_log("[settings] %s:%s", path.c_str(), summary.c_str());
}

int setting_int(const char* key) {
    std::lock_guard<std::mutex> lk(g_m);
    reload_if_changed_locked();
    auto it = g_values.find(key);
    if (it != g_values.end()) return std::atoi(it->second.c_str());
    for (const Option& o : kOptions)
        if (!std::string(key).compare(o.key)) return std::atoi(o.value);
    return 0;
}

bool setting_bool(const char* key) { return setting_int(key) != 0; }

}  // namespace tje
