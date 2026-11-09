// Call tracing for lifted code (debugging aid).
//
// TJE_TRACE=all            logs every lifted call up to TJE_TRACE_DEPTH (default 6) deep
// TJE_TRACE=13a0bf,151a20  logs only those functions, with arguments and result
// TJE_TRACE=under:10f400    logs every call made (directly or not) inside that function,
//                           up to TJE_TRACE_DEPTH (default 6) levels below it
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "guest.h"

namespace tje {

bool g_trace = false;
static std::set<uint32_t> g_trace_only;
static bool g_trace_all = false;
static bool g_trace_where = false;      // only remember the current function per thread
thread_local uint32_t t_where[8];        // recent function entries (ring)
thread_local uint32_t t_where_n = 0;
static int g_trace_depth_limit = 6;
static thread_local int t_depth = 0;
static uint32_t g_trace_under = 0;
static thread_local int t_under_depth = -1;   // t_depth at which g_trace_under was entered

void trace_init() {
    const char* spec = std::getenv("TJE_TRACE");
    if (!spec || !*spec) return;
    g_trace = true;
    if (!std::strncmp(spec, "under:", 6)) {
        g_trace_under = (uint32_t)std::strtoul(spec + 6, nullptr, 16);
    } else if (!std::strcmp(spec, "all")) {
        g_trace_all = true;
    } else if (!std::strcmp(spec, "where") || !std::strcmp(spec, "profile")) {
        g_trace_where = true;
    } else {
        std::string s = spec;
        size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            g_trace_only.insert((uint32_t)std::strtoul(s.substr(p, q - p).c_str(), nullptr, 16));
            p = q + 1;
        }
    }
    if (const char* d = std::getenv("TJE_TRACE_DEPTH")) g_trace_depth_limit = std::atoi(d);
}

static bool traced(uint32_t fn) {
    if (g_trace_under) return t_under_depth >= 0 && t_depth - t_under_depth <= g_trace_depth_limit;
    return g_trace_all ? t_depth <= g_trace_depth_limit : g_trace_only.count(fn) != 0;
}

volatile uint32_t g_profile_fn = 0;     // function most recently entered by any guest thread

void trace_enter(Cpu* c, uint32_t fn) {
    t_where[t_where_n++ & 7] = fn;
    g_profile_fn = fn;
    if (g_trace_where) return;
    t_depth++;
    if (g_trace_under == fn && t_under_depth < 0) t_under_depth = t_depth;
    if (traced(fn))
        hle_log("[call]%*s %08x (ecx %08x edx %08x | %08x %08x %08x %08x) from %08x", t_depth % 40, "", fn,
                c->ecx, c->edx, R32(c->esp + 4), R32(c->esp + 8), R32(c->esp + 12), R32(c->esp + 16), R32(c->esp));
}

void trace_leave(Cpu* c, uint32_t fn) {
    if (g_trace_where) return;
    if (traced(fn)) hle_log("[ret ]%*s %08x = %08x", t_depth % 40, "", fn, c->eax);
    if (t_depth == t_under_depth) t_under_depth = -1;
    t_depth--;
}

}  // namespace tje

namespace tje {
// For the watchdog: the calling thread's recent function entries, newest first.
std::string trace_where() {
    std::string out;
    for (uint32_t i = 0; i < 8 && i < t_where_n; i++) {
        char b[12];
        std::snprintf(b, sizeof b, " %08x", t_where[(t_where_n - 1 - i) & 7]);
        out += b;
    }
    return out;
}
}  // namespace tje

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>
#include <vector>

namespace tje {
// TJE_TRACE=profile: sample the most recently entered function every millisecond
// (only one guest thread runs at a time) and log the top 25 every 10 seconds.
uint64_t sample_running_host_pc();

// TJE_TRACE=hostprofile: sample the running thread's host code address every ms
// and log the top 40 addresses every 10 s (symbolize them with
// tools/recomp/symbolize_profile.py). Sees time in HLE and renderer code too.
static void start_host_profiler() {
    std::thread([] {
        std::map<uint64_t, uint64_t> hits;
        uint64_t total = 0;
        auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            hits[sample_running_host_pc()]++;
            total++;
            if (std::chrono::steady_clock::now() >= next_report) {
                std::vector<std::pair<uint64_t, uint64_t>> v;
                for (auto& kv : hits) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                std::string out;
                // Addresses in tje3.exe as offsets; elsewhere summed per module.
                std::map<std::string, uint64_t> modules;
                std::vector<std::pair<uint64_t, uint64_t>> in_exe;
#ifdef _WIN32
                uint64_t exe = (uint64_t)GetModuleHandle(nullptr);
                for (auto& [n, rip] : v) {
                    HMODULE m = nullptr;
                    char name[MAX_PATH] = "?";
                    if (rip && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                  (LPCSTR)rip, &m) && (uint64_t)m == exe)
                        in_exe.push_back({n, rip - exe});
                    else {
                        if (m) GetModuleFileNameA(m, name, sizeof name);
                        const char* base = std::strrchr(name, '\\');
                        modules[rip ? (base ? base + 1 : name) : "(idle)"] += n;
                    }
                }
#endif
                std::string mods;
                for (auto& [m, n] : modules) {
                    char b[300];
                    std::snprintf(b, sizeof b, " %s:%.1f%%", m.c_str(), 100.0 * n / total);
                    mods += b;
                }
                for (size_t i = 0; i < in_exe.size() && i < 60; i++) {
                    char b[48];
                    std::snprintf(b, sizeof b, " %llx:%.1f%%", (unsigned long long)in_exe[i].second, 100.0 * in_exe[i].first / total);
                    out += b;
                }
                hle_log("[hostprofile-modules]%s", mods.c_str());
                hle_log("[hostprofile]%s", out.c_str());
                hits.clear();
                total = 0;
                next_report += std::chrono::seconds(10);
            }
        }
    }).detach();
}

void start_profiler() {
    const char* spec = std::getenv("TJE_TRACE");
    if (spec && !std::strcmp(spec, "hostprofile")) { start_host_profiler(); return; }
    if (!spec || std::strcmp(spec, "profile")) return;
    std::thread([] {
        std::map<uint32_t, uint64_t> hits;
        uint64_t total = 0;
        auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            hits[(uint32_t)g_profile_fn]++;
            total++;
            if (std::chrono::steady_clock::now() >= next_report) {
                std::vector<std::pair<uint64_t, uint32_t>> v;
                for (auto& kv : hits) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                std::string out;
                for (size_t i = 0; i < v.size() && i < 25; i++) {
                    char b[40];
                    std::snprintf(b, sizeof b, " %08x:%.1f%%", v[i].second, 100.0 * v[i].first / total);
                    out += b;
                }
                hle_log("[profile]%s", out.c_str());
                hits.clear();
                total = 0;
                next_report += std::chrono::seconds(10);
            }
        }
    }).detach();
}
}  // namespace tje
