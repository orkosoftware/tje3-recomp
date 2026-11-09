// Core runtime: guest memory, function dispatch, guest threads, handles, waits.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "kobj.h"

namespace tje {

uint8_t* g_mem = nullptr;
FILE* g_log = stderr;

// --- logging -------------------------------------------------------------------
static std::mutex g_log_mutex;

bool log_enabled(const char* channel) {
    static const char* spec = std::getenv("TJE_LOG");
    if (!spec) return std::strcmp(channel, "warn") == 0;
    return std::strstr(spec, channel) != nullptr || std::strstr(spec, "all") != nullptr;
}

void hle_log(const char* fmt, ...) {
    std::lock_guard<std::mutex> g(g_log_mutex);
    static const auto start = std::chrono::steady_clock::now();
    std::fprintf(g_log, "%8.3f ", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void hle_unimplemented(const char* name, Cpu* c) {
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> g(g_log_mutex);
    if (seen.insert(name).second)
        std::fprintf(g_log, "[hle] unimplemented %s (called from %08x)\n", name, R32(c->esp));
    std::fflush(g_log);
}

// --- memory ------------------------------------------------------------------------
#ifdef _WIN32
void log_backtrace(Cpu* c, const char* why);
static void report_host_crash(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return;
    std::fprintf(g_log, "[crash] host exception %08lx at %p (module %p, guest memory %p) data %p\n", code,
                 e->ExceptionRecord->ExceptionAddress, (void*)GetModuleHandle(nullptr), (void*)g_mem,
                 (void*)(code == EXCEPTION_ACCESS_VIOLATION ? e->ExceptionRecord->ExceptionInformation[1] : 0));
    std::fprintf(g_log, "[crash] host offset +%llx (llvm-symbolizer --obj=tje3.exe --relative-address)\n",
                 (unsigned long long)((uint8_t*)e->ExceptionRecord->ExceptionAddress - (uint8_t*)GetModuleHandle(nullptr)));
    if (GuestThread* t = current_thread()) {
        std::fprintf(g_log, "[crash] in guest thread %u\n", t->id);
        log_backtrace(&t->cpu, "crash");
    }
    std::fflush(g_log);
}

// Unhandled host exceptions (including stack overflows, given the stack
// guarantee each guest thread sets) are logged and written to a minidump by a
// thread started in advance, so the crashing thread needs little stack.
static std::string g_dump_path;
static HANDLE g_dump_request = nullptr, g_dump_done = nullptr;
static EXCEPTION_POINTERS* g_dump_info = nullptr;
static DWORD g_dump_thread_id = 0;

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    std::fprintf(g_log, "[crash] unhandled host exception %08lx at host offset +%llx (thread %lu)\n", code,
                 (unsigned long long)((uint8_t*)e->ExceptionRecord->ExceptionAddress - (uint8_t*)GetModuleHandle(nullptr)),
                 GetCurrentThreadId());
    if (GuestThread* t = current_thread()) {
        std::fprintf(g_log, "[crash] in guest thread %u (esp %08x)\n", t->id, t->cpu.esp);
        log_backtrace(&t->cpu, "crash");
    }
    std::fflush(g_log);
    if (g_dump_request) {
        g_dump_info = e;
        g_dump_thread_id = GetCurrentThreadId();
        SetEvent(g_dump_request);
        WaitForSingleObject(g_dump_done, 30000);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void crash_capture_init(const std::string& dump_path) {
    g_dump_path = dump_path;
    g_dump_request = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_dump_done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    std::thread([] {
        WaitForSingleObject(g_dump_request, INFINITE);
        using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
#pragma pack(push, 4)                        // as MINIDUMP_EXCEPTION_INFORMATION in dbghelp.h
        struct ExceptionInfo { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };
#pragma pack(pop)
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        auto write = dbghelp ? (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
        HANDLE f = CreateFileA(g_dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (write && f != INVALID_HANDLE_VALUE) {
            ExceptionInfo info{g_dump_thread_id, g_dump_info, FALSE};
            // MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory
            bool ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000 | 0x40, &info, nullptr, nullptr);
            hle_log("[crash] minidump %s %s (error %lu)", g_dump_path.c_str(), ok ? "written" : "failed", ok ? 0ul : GetLastError());
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        SetEvent(g_dump_done);
    }).detach();
    SetUnhandledExceptionFilter(unhandled_filter);
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
}

// Debugging: pages made read-only by mem_watch_writes() report the first write
// to them (host code address, for llvm-symbolizer) and become writable again.
static std::mutex g_watch_m;
static std::set<uint32_t> g_watched_pages;

// mem_watch_dword(): every write to one dword is logged (page kept read-only,
// re-armed after each faulting instruction by single-stepping it).
static uint32_t g_watch_dword = 0;
static thread_local bool t_rearm = false;

static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
        t_rearm = false;
        DWORD old;
        VirtualProtect(g_mem + (g_watch_dword & ~0xfffu), 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) { report_host_crash(e); return EXCEPTION_CONTINUE_SEARCH; }
    if (g_watch_dword && e->ExceptionRecord->ExceptionInformation[0] == 1) {
        uint8_t* wa = (uint8_t*)e->ExceptionRecord->ExceptionInformation[1];
        if (wa >= g_mem && wa < g_mem + 0x100000000ull &&
            ((uint32_t)(wa - g_mem) & ~0xfffu) == (g_watch_dword & ~0xfffu)) {
            uint32_t ga = (uint32_t)(wa - g_mem);
            if (ga + 8 > g_watch_dword && ga < g_watch_dword + 4)
                hle_log("[watch] write to %08x (old %08x) from host +%llx", ga, R32(g_watch_dword),
                        (unsigned long long)((uint8_t*)e->ExceptionRecord->ExceptionAddress - (uint8_t*)GetModuleHandle(nullptr)));
            DWORD old;
            VirtualProtect(g_mem + (g_watch_dword & ~0xfffu), 0x1000, PAGE_READWRITE, &old);
            e->ContextRecord->EFlags |= 0x100;      // trap after this instruction, then re-protect
            t_rearm = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    uint8_t* a = (uint8_t*)e->ExceptionRecord->ExceptionInformation[1];
    if (a < g_mem || a >= g_mem + 0x100000000ull) { report_host_crash(e); return EXCEPTION_CONTINUE_SEARCH; }
    uint32_t ga = (uint32_t)(a - g_mem);
    if (e->ExceptionRecord->ExceptionInformation[0] == 1) {
        std::lock_guard<std::mutex> lk(g_watch_m);
        if (g_watched_pages.erase(ga & ~0xfffu)) {
            DWORD old;
            VirtualProtect(a - (ga & 0xfff), 0x1000, PAGE_READWRITE, &old);
            hle_log("[watch] write to guest %08x from host %p (module %p)", ga, e->ExceptionRecord->ExceptionAddress,
                    (void*)GetModuleHandle(nullptr));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (log_enabled("touch") || ga < 0x10000 || (ga >= 0xfd000000 && ga < 0xfe000000))
        hle_log("[mem] first touch of guest %08x%s", ga, ga < 0x10000 ? " (null page)" : "");
    if (!VirtualAlloc((void*)((uintptr_t)a & ~(uintptr_t)0xffff), 0x10000, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

void mem_watch_writes(uint32_t addr, uint32_t len) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(g_watch_m);
    for (uint32_t p = addr & ~0xfffu; p < addr + len; p += 0x1000) {
        DWORD old;
        if (VirtualProtect(g_mem + p, 0x1000, PAGE_READONLY, &old)) g_watched_pages.insert(p);
    }
#endif
}

void mem_watch_dword(uint32_t addr) {
#ifdef _WIN32
    g_watch_dword = addr;
    DWORD old;
    VirtualProtect(g_mem + (addr & ~0xfffu), 0x1000, PAGE_READONLY, &old);
#endif
}

void mem_init() {
#ifdef _WIN32
    g_mem = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE);
    AddVectoredExceptionHandler(1, commit_on_touch);
#else
    g_mem = (uint8_t*)mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#endif
    if (!g_mem) { std::fprintf(stderr, "cannot reserve guest memory\n"); std::exit(1); }
}

// First-fit allocator over one guest region, with coalescing free list.
class Region {
public:
    Region(uint32_t lo, uint32_t hi) { free_[lo] = hi - lo; }
    uint32_t alloc(uint32_t size, uint32_t align) {
        size = (size + 15) & ~15u;
        if (!size) size = 16;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            uint32_t a = (it->first + align - 1) & ~(align - 1);
            uint32_t end = it->first + it->second;
            if (a + (uint64_t)size > end) continue;
            uint32_t lo = it->first;
            free_.erase(it);
            if (a > lo) free_[lo] = a - lo;
            if (a + size < end) free_[a + size] = end - (a + size);
            used_[a] = size;
            std::memset(g_mem + a, 0, size);
            return a;
        }
        return 0;
    }
    bool owns(uint32_t a) const { return used_.count(a) != 0; }
    uint32_t size(uint32_t a) const { auto it = used_.find(a); return it == used_.end() ? 0 : it->second; }
    void release(uint32_t a) {
        auto it = used_.find(a);
        if (it == used_.end()) return;
        uint32_t lo = a, len = it->second;
        used_.erase(it);
        auto nx = free_.lower_bound(lo);
        if (nx != free_.end() && nx->first == lo + len) { len += nx->second; free_.erase(nx); }
        auto pv = free_.lower_bound(lo);
        if (pv != free_.begin()) {
            --pv;
            if (pv->first + pv->second == lo) { lo = pv->first; len += pv->second; free_.erase(pv); }
        }
        free_[lo] = len;
    }
private:
    std::map<uint32_t, uint32_t> free_, used_;
};

static std::mutex g_alloc_mutex;
static Region g_pool(kPoolBase, kPoolEnd), g_contig(kContigBase + 0x100000, kContigEnd);

uint32_t guest_alloc(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> g(g_alloc_mutex);
    uint32_t a = g_pool.alloc(size, align < 16 ? 16 : align);
    if (!a) hle_log("[mem] pool exhausted (%u bytes)", size);
    return a;
}
uint32_t guest_alloc_contiguous(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> g(g_alloc_mutex);
    uint32_t a = g_contig.alloc(size, align < 16 ? 16 : align);
    if (!a) hle_log("[mem] contiguous memory exhausted (%u bytes)", size);
    return a;
}
uint32_t guest_alloc_size(uint32_t a) {
    std::lock_guard<std::mutex> g(g_alloc_mutex);
    return g_pool.owns(a) ? g_pool.size(a) : g_contig.size(a);
}
void guest_free(uint32_t a) {
    std::lock_guard<std::mutex> g(g_alloc_mutex);
    if (g_pool.owns(a)) g_pool.release(a);
    else g_contig.release(a);
}

std::string guest_cstr(uint32_t a, size_t max) {
    std::string s;
    while (s.size() < max && g_mem[a + s.size()]) s.push_back((char)g_mem[a + s.size()]);
    return s;
}

// --- dispatch ----------------------------------------------------------------------------
static std::unordered_map<uint32_t, GuestFn>& registry() {
    static std::unordered_map<uint32_t, GuestFn> r;
    return r;
}
void register_fn(uint32_t addr, GuestFn fn) { registry()[addr] = fn; }
GuestFn lookup(uint32_t addr) {
    auto& r = registry();
    auto it = r.find(addr);
    return it == r.end() ? nullptr : it->second;
}

struct GuestFault : std::runtime_error { using std::runtime_error::runtime_error; };
struct ThreadExit { uint32_t code; };

[[noreturn]] void fault(const char* what, uint32_t addr) {
    char msg[200];
    std::snprintf(msg, sizeof msg, "%s at %08x", what, addr);
    throw GuestFault(msg);
}

void call_indirect(Cpu* c, uint32_t target) {
    GuestFn f = lookup(target);
    if (!f) fault("call to unknown address", target);
    f(c);
}

// --- handles --------------------------------------------------------------------------------
static std::map<uint32_t, KObject*> g_handles;
static uint32_t g_next_handle = 0x100;

uint32_t handle_new(KObject* o) {
    uint32_t h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = o;
    return h;
}
KObject* handle_get(uint32_t h) {
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}
bool handle_close(uint32_t h) {
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return false;
    KObject* o = it->second;
    g_handles.erase(it);
    if (o->fp) { std::fclose(o->fp); o->fp = nullptr; }
    if (o->type != ObjType::Thread) delete o;      // thread objects live as long as the thread
    return true;
}

size_t handle_open_files() {
    size_t n = 0;
    for (auto& [h, o] : g_handles) n += o->fp != nullptr;
    return n;
}

int64_t now_100ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100;
}

bool kobj_signaled(KObject* o, uint32_t waiter) {
    switch (o->type) {
    case ObjType::Event: case ObjType::Timer: return o->signaled;
    case ObjType::Semaphore: return o->count > 0;
    case ObjType::Mutant: return o->owner == 0 || o->owner == waiter;
    case ObjType::Thread: return o->thread == nullptr || o->thread->done;
    default: return true;
    }
}
void kobj_acquire(KObject* o, uint32_t waiter) {
    switch (o->type) {
    case ObjType::Event: case ObjType::Timer: if (!o->manual_reset) o->signaled = false; break;
    case ObjType::Semaphore: o->count--; break;
    case ObjType::Mutant: o->owner = waiter; o->recursion++; break;
    default: break;
    }
}

// Fatal guest fault: report and stop the process.
void log_backtrace(Cpu* c, const char* why);
void report_fault(GuestThread* t, const char* what) {
    hle_log("[fault] thread %u: %s", t->id, what);
    log_backtrace(&t->cpu, "fault");
    std::fflush(g_log);
    std::_Exit(3);
}

// Poor man's backtrace: stack words that are return addresses (the byte before
// them ends a call instruction in .text).
void log_backtrace(Cpu* c, const char* why) {
    std::string out;
    int n = 0;
    for (uint32_t a = c->esp; a < c->esp + 0x2000 && n < 24; a += 4) {
        uint32_t v = R32(a);
        if (v < 0x11005 || v >= 0x161100) continue;
        bool call = R8(v - 5) == 0xe8 || (R8(v - 6) == 0xff && (R8(v - 5) & 0x38) == 0x10) ||
                    (R8(v - 2) == 0xff && (R8(v - 1) & 0x38) == 0x10) || (R8(v - 3) == 0xff && (R8(v - 2) & 0x38) == 0x10);
        if (!call) continue;
        char buf[16];
        std::snprintf(buf, sizeof buf, " %08x", v);
        out += buf;
        n++;
    }
    hle_log("[trace] %s:%s", why, out.c_str());
}
// Calls a lifted (or HLE) guest function from HLE code with stack arguments; works for
// stdcall and cdecl alike because esp is restored afterwards. Returns eax.
uint32_t guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t saved = c->esp;
    std::vector<uint32_t> v(args);
    for (size_t i = v.size(); i-- > 0;) { c->esp -= 4; W32(c->esp, v[i]); }
    c->esp -= 4;
    W32(c->esp, 0);
    GuestFn f = lookup(fn);
    if (!f) fault("guest_call to unknown function", fn);
    f(c);
    c->esp = saved;
    return c->eax;
}
void note_hle_call(Cpu* c, const char* name) {
    GuestThread* t = current_thread();
    if (!t) return;
    t->hle_calls++;
    t->last_hle_from = R32(c->esp);
    (void)name;
}
}  // namespace tje

namespace tje {
uint64_t guest_rdtsc() {
    static const auto start = std::chrono::steady_clock::now();
    uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start).count();
    return ns * 11 / 15;          // 733.33 MHz
}
}  // namespace tje
