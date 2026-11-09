// Kernel objects behind guest handles (events, semaphores, files, threads...).
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <vector>

#include "guest.h"

namespace tje {

struct GuestThread {
    Cpu cpu{};
    uint32_t kpcr = 0, kthread = 0, handle = 0, id = 0;
    uint32_t stack_lo = 0, stack_hi = 0;
    uint32_t routine = 0, arg1 = 0, arg2 = 0, tls_size = 0;
    bool suspended = false, done = false;          // suspended: created suspended, not started yet
    int suspend_count = 0;                          // NtSuspendThread / NtResumeThread
    uint32_t exit_code = 0;
    std::condition_variable sched_cv;              // signalled when it may run
    bool in_blocked = false;                       // in the scheduler's blocked list
    std::thread host;
    void* host_handle = nullptr;                    // Windows thread handle (elsewhere: non-null once started)
#ifndef _WIN32
    pthread_t host_pthread{};
#endif
    const char* blocked = nullptr;                  // what it waits for (watchdog)
    uint32_t last_hle_from = 0;                     // return address of its last HLE call
    uint64_t hle_calls = 0;
    double ready_wait_ms = 0;                       // time spent ready but waiting for the CPU
    char where[96] = {0};                           // recent functions (TJE_TRACE=where)
    // User-mode APCs, run when the thread waits alertably: routine(context, arg1, arg2), stdcall.
    struct Apc { uint32_t routine, context, arg1, arg2; };
    std::mutex apc_m;
    std::deque<Apc> apcs;
};

struct KObject {
    ObjType type = ObjType::Other;
    // event
    bool manual_reset = false, signaled = false;
    // semaphore
    int32_t count = 0, limit = 0;
    // mutant
    uint32_t owner = 0;
    int32_t recursion = 0;
    // timer
    int64_t due = 0;       // host steady-clock ticks (100 ns), 0 = not set
    int32_t period_ms = 0;
    // file / directory
    FILE* fp = nullptr;
    std::string host_path, guest_path;
    bool is_dir = false;
    std::vector<std::string> dir_entries;
    size_t dir_pos = 0;
    uint64_t pos = 0;
    std::vector<uint8_t> read_overlay;      // replaces the start of the file when read
    // thread
    GuestThread* thread = nullptr;
};

// Is the object signaled for a waiter (without consuming it)?
bool kobj_signaled(KObject* o, uint32_t waiter);
// Consume the signal after a successful wait (auto-reset events, semaphores, mutants).
void kobj_acquire(KObject* o, uint32_t waiter);

// Blocks the calling guest thread until ready() holds; timeout_100ns < 0 = infinite.
bool wait_until(const std::function<bool()>& ready, int64_t timeout_100ns);
int64_t now_100ns();       // host monotonic time in 100 ns units
void park_if_suspended();

}  // namespace tje
