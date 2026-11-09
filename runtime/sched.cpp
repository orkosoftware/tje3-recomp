// Guest thread scheduler: a single-CPU kernel scheduler on top of host threads.
//
// Each guest thread runs on its own host thread (deep host stacks for deep guest
// recursion), but exactly one of them runs at a time, like the Xbox's single CPU.
// The running thread hands the CPU over explicitly: when it blocks, yields
// (Sleep(0), preemption at a yield point) or waits on the host. As in the NT
// kernel, the highest-priority ready thread runs next (FIFO among equals), and
// Sleep(0) or the end of a time slice only gives way to threads of at least the
// same priority; a higher-priority thread that becomes ready preempts at the
// next yield point. Priorities are the KeSetBasePriorityThread increments kept
// in KTHREAD+0x200. Blocked threads re-check their wait condition whenever a
// kernel object changes (sched_wake_all) or their timeout expires.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __ANDROID__
#include <sched.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#endif

#include "kobj.h"

namespace tje {

struct ThreadExit { uint32_t code; };
struct GuestFault;
[[noreturn]] void fault(const char* what, uint32_t addr);

extern volatile uint32_t g_profile_fn;
std::string trace_where();

namespace {
std::mutex g_m;                                   // protects everything below
GuestThread* g_running = nullptr;
std::deque<GuestThread*> g_ready;
std::vector<GuestThread*> g_blocked;
std::vector<GuestThread*> g_all;
thread_local GuestThread* t_current = nullptr;
uint32_t g_next_tid = 1;
uint32_t g_pcr_next = kKernelData + 0x100000;

// --- core (g_m held) ------------------------------------------------------------
int priority(const GuestThread* t) { return (int32_t)R32(t->kthread + 0x200); }

// Index of the first ready thread with the highest priority (-1 if none).
int best_ready_locked() {
    int best = -1;
    for (size_t i = 0; i < g_ready.size(); i++)
        if (best < 0 || priority(g_ready[i]) > priority(g_ready[best])) best = (int)i;
    return best;
}
void make_ready_locked(GuestThread* t) {
    if (!g_running) { g_running = t; t->sched_cv.notify_one(); return; }
    g_ready.push_back(t);
    if (priority(t) > priority(g_running)) g_yield_request = true;      // preempt at the next yield point
}
void hand_off_locked() {
    int i = best_ready_locked();
    if (i >= 0) {
        g_running = g_ready[i];
        g_ready.erase(g_ready.begin() + i);
        g_running->sched_cv.notify_one();
    } else {
        g_running = nullptr;
    }
}
void wait_turn_locked(std::unique_lock<std::mutex>& lk, GuestThread* t) {
    auto t0 = std::chrono::steady_clock::now();
    t->sched_cv.wait(lk, [&] { return g_running == t; });
    t->ready_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

GuestThread* current_thread() { return t_current; }
Cpu* current_cpu() { return &t_current->cpu; }
uint32_t current_kthread() { return t_current ? t_current->kthread : 0; }

// Conditions of blocked threads may have changed: let them re-check.
void sched_wake_all() {
    std::lock_guard<std::mutex> lk(g_m);
    for (GuestThread* t : g_blocked) { t->in_blocked = false; make_ready_locked(t); }
    g_blocked.clear();
}

// Lets the other ready threads run first (Sleep(0), NtYieldExecution, preemption).
void sched_yield() {
    GuestThread* t = t_current;
    std::unique_lock<std::mutex> lk(g_m);
    int i = best_ready_locked();
    if (i < 0 || priority(g_ready[i]) < priority(t)) return;   // nobody of at least our priority
    g_ready.push_back(t);
    hand_off_locked();
    wait_turn_locked(lk, t);
}

// Gives up the CPU while the host waits (vblank pacing, sleeps), then takes it back.
void without_gil(const std::function<void()>& fn) {
    GuestThread* t = t_current;
    g_profile_fn = 0xffffffffu;
    {
        std::lock_guard<std::mutex> lk(g_m);
        hand_off_locked();
    }
    fn();
    std::unique_lock<std::mutex> lk(g_m);
    make_ready_locked(t);
    wait_turn_locked(lk, t);
    lk.unlock();
    park_if_suspended();
}

// Blocks until ready() holds (checked while running) or the timeout passes.
bool wait_until(const std::function<bool()>& ready, int64_t timeout_100ns) {
    GuestThread* t = t_current;
    int64_t deadline = timeout_100ns < 0 ? -1 : now_100ns() + timeout_100ns;
    if (!t->blocked) t->blocked = "wait";
    struct Clear { GuestThread* t; ~Clear() { t->blocked = nullptr; } } clear{t};
    for (;;) {
        if (t->suspend_count == 0 && ready()) return true;
        if (deadline >= 0 && now_100ns() >= deadline) return false;
        g_profile_fn = 0xffffffffu;
        std::unique_lock<std::mutex> lk(g_m);
        t->in_blocked = true;
        g_blocked.push_back(t);
        hand_off_locked();
        while (g_running != t) {
            if (deadline >= 0) {
                auto until = std::chrono::steady_clock::now() + std::chrono::microseconds((deadline - now_100ns()) / 10 + 1);
                if (t->sched_cv.wait_until(lk, until) == std::cv_status::timeout && g_running != t &&
                    t->in_blocked) {
                    for (size_t i = 0; i < g_blocked.size(); i++)
                        if (g_blocked[i] == t) { g_blocked.erase(g_blocked.begin() + i); break; }
                    t->in_blocked = false;
                    make_ready_locked(t);
                }
            } else {
                t->sched_cv.wait(lk);
            }
        }
    }
}

// A thread another thread suspended stops at its next kernel call or yield point.
void park_if_suspended() {
    GuestThread* t = t_current;
    if (t && t->suspend_count > 0) wait_until([] { return true; }, -1);
}

// --- preemption ---------------------------------------------------------------------
volatile bool g_yield_request = false;

void yield_point(Cpu* c) {
    (void)c;
    g_yield_request = false;
    GuestThread* t = t_current;
    if (!t) return;
    if (g_trace) std::snprintf(t->where, sizeof t->where, "%s", trace_where().c_str());
    sched_yield();
    park_if_suspended();
}

void start_preemption_timer() {
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));   // ~Xbox-like time slices
            g_yield_request = true;
        }
    }).detach();
}

// --- thread lifetime -------------------------------------------------------------------
static void setup_thread(GuestThread* t, uint32_t stack_size, uint32_t tls_size) {
    stack_size = (stack_size + 0xffff) & ~0xffffu;
    if (stack_size < 0x10000) stack_size = 0x10000;
    tls_size = (tls_size + 15) & ~15u;
    t->tls_size = tls_size;
    t->stack_lo = guest_alloc(stack_size + tls_size, 0x1000);
    t->stack_hi = t->stack_lo + stack_size + tls_size;
    uint32_t tls = t->stack_hi - tls_size;
    t->kpcr = g_pcr_next;
    g_pcr_next += 0x1000;
    t->kthread = t->kpcr + 0x400;
    std::memset(g_mem + t->kpcr, 0, 0x1000);
    W32(t->kpcr + 0x00, 0xffffffffu);       // NtTib.ExceptionList
    W32(t->kpcr + 0x04, t->stack_hi);       // NtTib.StackBase (TLS sits just below it)
    W32(t->kpcr + 0x08, t->stack_lo);       // NtTib.StackLimit
    W32(t->kpcr + 0x18, t->kpcr);           // NtTib.Self
    W32(t->kpcr + 0x1c, t->kpcr);           // SelfPcr
    W32(t->kpcr + 0x20, t->kpcr + 0x28);    // Prcb
    W8(t->kpcr + 0x24, 0);                  // Irql
    W32(t->kpcr + 0x28, t->kthread);        // Prcb.CurrentThread
    W32(t->kthread + 0x28, tls);            // KTHREAD.TlsData
    t->id = g_next_tid++;
    W32(t->kthread + 0x130, t->id);         // ETHREAD.UniqueThread
    W8(t->kthread, 6);                      // DISPATCHER_HEADER.Type: ThreadObject
    t->cpu.fs_base = t->kpcr;
    t->cpu.fcw = 0x027f;
    t->cpu.esp = (tls - 16) & ~15u;
}

void report_fault(GuestThread* t, const char* what);

static void run_guest(GuestThread* t, uint32_t routine, bool two_args) {
    Cpu* c = &t->cpu;
    try {
        GuestFn f = lookup(routine);
        if (!f) fault("thread routine not found", routine);
        if (two_args) {
            c->esp -= 4; W32(c->esp, t->arg2);
            c->esp -= 4; W32(c->esp, t->arg1);
        }
        c->esp -= 4; W32(c->esp, 0);       // no return address: the routine exits the thread
        f(c);
        t->exit_code = c->eax;
    } catch (ThreadExit& e) {
        t->exit_code = e.code;
    } catch (std::exception& e) {
        report_fault(t, e.what());
    }
}

#ifdef __ANDROID__
// Phones and handhelds mix slow and fast cores. One guest thread runs at a time
// and it needs a fast one, so keep the game off the slowest group.
static void pin_to_fast_cores() {
    if (std::getenv("TJE_NO_PIN")) return;
    static const cpu_set_t fast = [] {
        std::vector<long> freq;
        for (int i = 0; i < 32; i++) {
            std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq";
            FILE* f = std::fopen(p.c_str(), "r");
            if (!f) break;
            long v = 0;
            if (std::fscanf(f, "%ld", &v) != 1) v = 0;
            std::fclose(f);
            freq.push_back(v);
        }
        long lo = freq.empty() ? 0 : *std::min_element(freq.begin(), freq.end());
        long hi = freq.empty() ? 0 : *std::max_element(freq.begin(), freq.end());
        cpu_set_t set;
        CPU_ZERO(&set);
        for (size_t i = 0; i < freq.size(); i++)
            if (freq[i] > lo || lo == hi) CPU_SET(i, &set);
        hle_log("[sched] %d of %zu cores in use", CPU_COUNT(&set), freq.size());
        return set;
    }();
    if (CPU_COUNT(&fast)) sched_setaffinity(0, sizeof fast, &fast);
}
#else
static void pin_to_fast_cores() {}
#endif

static void thread_body(GuestThread* t) {
    t_current = t;
    pin_to_fast_cores();
#ifdef _WIN32
    ULONG guarantee = 64 * 1024;             // room to report a stack overflow
    SetThreadStackGuarantee(&guarantee);
#endif
    {
        std::unique_lock<std::mutex> lk(g_m);
        make_ready_locked(t);
        wait_turn_locked(lk, t);
    }
    if (t->suspended) wait_until([t] { return !t->suspended; }, -1);
    run_guest(t, t->routine, true);
    t->done = true;
    W8(t->kthread + 4, 1);                   // DISPATCHER_HEADER.SignalState: terminated
    W32(t->kthread + 0x120, t->exit_code);   // ETHREAD.ExitStatus (GetExitCodeThread)
    if (log_enabled("thread")) hle_log("[thread] %u exited with %u", t->id, t->exit_code);
    sched_wake_all();                        // threads waiting on this one
    std::lock_guard<std::mutex> lk(g_m);
    hand_off_locked();
}

// Guest recursion is host recursion (each lifted frame is larger than the original),
// so guest threads get 256 MB host stacks (reserved address space, committed on use).
static void start_host_thread(GuestThread* t) {
#ifdef _WIN32
    t->host_handle = CreateThread(nullptr, 256u << 20,
                                  [](LPVOID p) -> DWORD { thread_body((GuestThread*)p); return 0; }, t,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 20);
    pthread_create(&t->host_pthread, &attr, [](void* p) -> void* { thread_body((GuestThread*)p); return nullptr; }, t);
    pthread_attr_destroy(&attr);
    t->host_handle = t;
#endif
}

uint32_t create_thread(uint32_t routine, uint32_t arg1, uint32_t arg2, uint32_t stack_size,
                       uint32_t tls_size, bool suspended, uint32_t* thread_id) {
    auto* t = new GuestThread();
    setup_thread(t, stack_size, tls_size);
    t->routine = routine; t->arg1 = arg1; t->arg2 = arg2; t->suspended = suspended;
    auto* o = new KObject();
    o->type = ObjType::Thread;
    o->thread = t;
    t->handle = handle_new(o);
    if (thread_id) *thread_id = t->id;
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_all.push_back(t);
    }
    if (log_enabled("thread"))
        hle_log("[thread] create %u: routine %08x(%08x, %08x) stack %x tls %x", t->id, routine, arg1, arg2,
                stack_size, tls_size);
    start_host_thread(t);
    return t->handle;
}

void resume_thread(uint32_t handle) {
    KObject* o = handle_get(handle);
    if (o && o->thread) { o->thread->suspended = false; sched_wake_all(); }
}

// Returns the previous suspend count, or -1 for a bad handle.
int suspend_thread(uint32_t handle, int delta) {
    KObject* o = handle_get(handle);
    if (!o || !o->thread) return -1;
    GuestThread* t = o->thread;
    int prev = t->suspend_count;
    if (delta < 0) {
        if (t->suspended) { t->suspended = false; prev = 1; }      // created suspended
        else if (t->suspend_count > 0) t->suspend_count--;
    } else {
        t->suspend_count++;
    }
    sched_wake_all();
    return prev;
}

[[noreturn]] void exit_thread(uint32_t code) { throw ThreadExit{code}; }

void run_boot_thread(uint32_t entry) {
    auto* t = new GuestThread();
    setup_thread(t, 0x40000, 0);
    t_current = t;
    pin_to_fast_cores();
    {
        std::unique_lock<std::mutex> lk(g_m);
        g_all.push_back(t);
        make_ready_locked(t);
        wait_turn_locked(lk, t);
    }
    run_guest(t, entry, false);
    t->done = true;
    hle_log("[boot] entry point returned");
    sched_wake_all();
    std::lock_guard<std::mutex> lk(g_m);
    hand_off_locked();
}

void wait_for_all_threads() {
    for (size_t i = 0;; i++) {
        GuestThread* t;
        {
            std::lock_guard<std::mutex> lk(g_m);
            if (i >= g_all.size()) break;
            t = g_all[i];
        }
        if (!t->host_handle && !t->host.joinable()) continue;     // the boot thread
#ifdef _WIN32
        WaitForSingleObject(t->host_handle, INFINITE);
#else
        pthread_join(t->host_pthread, nullptr);
#endif
    }
}

// TJE_WATCH=<seconds>: periodically log what every guest thread is doing.
// Host code address where the running guest thread is now; 0 if none.
uint64_t sample_running_host_pc() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(g_m);
    GuestThread* t = g_running;
    if (!t || !t->host_handle || SuspendThread((HANDLE)t->host_handle) == (DWORD)-1) return 0;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    uint64_t rip = GetThreadContext((HANDLE)t->host_handle, &ctx) ? ctx.Rip : 0;
    ResumeThread((HANDLE)t->host_handle);
    return rip;
#else
    return 0;
#endif
}

// One line per guest thread (state, what it waits for, host code address of the
// running one); with backtraces, each thread's guest return addresses too.
void report_threads(const char* why, bool backtraces) {
    void log_backtrace(Cpu*, const char*);
    std::string out;
    std::lock_guard<std::mutex> lk(g_m);
    for (GuestThread* t : g_all) {
        char buf[320];
        const char* st = t->done ? "done" : t == g_running ? "RUNNING"
                       : t->in_blocked ? "blocked" : t->suspend_count ? "suspended" : "ready";
        unsigned long long rip = 0;
#ifdef _WIN32
        // The running thread's host code address (llvm-symbolizer on tje3.exe names the lifted function).
        if (t == g_running && t->host_handle && SuspendThread((HANDLE)t->host_handle) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext((HANDLE)t->host_handle, &ctx)) rip = ctx.Rip - (unsigned long long)GetModuleHandle(nullptr);
            ResumeThread((HANDLE)t->host_handle);
        }
#endif
        std::snprintf(buf, sizeof buf, " [t%u %s%s%s esp %08x last-hle-from %08x host +%llx in%s]", t->id, st,
                      t->blocked ? ":" : "", t->blocked ? t->blocked : "", t->cpu.esp, t->last_hle_from,
                      rip, t->where);
        out += buf;
        if (backtraces && !t->done) {
            char label[64];
            std::snprintf(label, sizeof label, "%s t%u", why, t->id);
            log_backtrace(&t->cpu, label);
        }
        if (backtraces && t == g_running) {
            hle_log("[%s] t%u eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x", why, t->id, t->cpu.eax,
                    t->cpu.ebx, t->cpu.ecx, t->cpu.edx, t->cpu.esi, t->cpu.edi, t->cpu.ebp);
            // Memory the registers point at (guest heap and stack structures).
            for (uint32_t a : {t->cpu.eax, t->cpu.ebx, t->cpu.esi, t->cpu.edi}) {
                if (a < 0x10000 || a >= 0x10000000) continue;
                uint32_t b = (a & ~15u) - 16;
                for (int k = 0; k < 3; k++)
                    hle_log("[%s]   %08x: %08x %08x %08x %08x", why, b + 16 * k, R32(b + 16 * k), R32(b + 16 * k + 4),
                            R32(b + 16 * k + 8), R32(b + 16 * k + 12));
            }
        }
    }
    hle_log("[%s]%s", why, out.c_str());
}

// TJE_WATCH=<seconds>: periodically log what every guest thread is doing.
void start_watchdog() {
    const char* w = std::getenv("TJE_WATCH");
    if (!w) return;
    int secs = std::atoi(w) > 0 ? std::atoi(w) : 5;
    std::thread([secs] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            report_threads("watch", log_enabled("regs"));
        }
    }).detach();
}

}  // namespace tje
