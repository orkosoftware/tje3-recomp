// Runtime services for the recompiled game: guest memory, guest threads,
// kernel objects and helpers for writing HLE (high-level emulation) functions.
//
// Threads: every guest thread is a host thread with its own Cpu; the scheduler
// (sched.cpp) lets exactly one run at a time, like the single-core Xbox.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <functional>
#include <initializer_list>
#include <string>

#include "cpu.h"

namespace tje {

// --- memory --------------------------------------------------------------------
void mem_init();                                   // reserve the 4 GB guest space
void mem_watch_writes(uint32_t addr, uint32_t len); // debugging: log the first write to each page
void mem_watch_dword(uint32_t addr);                // debugging: log every write to one dword
uint32_t guest_alloc(uint32_t size, uint32_t align = 16);       // general (pool / virtual)
uint32_t guest_alloc_contiguous(uint32_t size, uint32_t align = 0x1000);   // "physical" memory
uint32_t guest_alloc_size(uint32_t addr);          // size of a guest_alloc block (0 if unknown)
void guest_free(uint32_t addr);
inline char* gptr(uint32_t a) { return (char*)(g_mem + a); }
std::string guest_cstr(uint32_t a, size_t max = 4096);

// Guest memory regions.
constexpr uint32_t kPoolBase = 0x01000000, kPoolEnd = 0x70000000;
constexpr uint32_t kContigBase = 0x80000000, kContigEnd = 0x84000000;   // 64 MB, like the Xbox
constexpr uint32_t kKernelData = 0x8f000000;       // kernel data exports, KPCRs
constexpr uint32_t kKernelThunks = 0xfe000000;     // fake addresses of kernel functions

// --- threads and the global lock ----------------------------------------------------
// The scheduler (sched.cpp): one guest thread runs at a time.
void sched_wake_all();                             // kernel objects changed: blocked threads re-check
void sched_yield();                                // Sleep(0): let ready threads run first
struct GuestThread;
GuestThread* current_thread();
Cpu* current_cpu();
uint32_t current_kthread();                        // guest KTHREAD address of the running thread

// Creates a guest thread running `routine(arg1, arg2)` (stdcall). Returns its handle.
uint32_t create_thread(uint32_t routine, uint32_t arg1, uint32_t arg2, uint32_t stack_size,
                       uint32_t tls_size, bool suspended, uint32_t* thread_id);
void resume_thread(uint32_t handle);
[[noreturn]] void exit_thread(uint32_t code);
void run_boot_thread(uint32_t entry);              // runs the XBE entry on the calling host thread
void wait_for_all_threads();

// --- kernel object handles -------------------------------------------------------------
enum class ObjType { Event, Semaphore, Mutant, Timer, Thread, File, IoCompletion, Other };
struct KObject;
uint32_t handle_new(KObject* o);
KObject* handle_get(uint32_t h);
bool handle_close(uint32_t h);
size_t handle_open_files();       // handles holding an open host file

// --- HLE helpers ---------------------------------------------------------------------------
inline uint32_t A(Cpu* c, int i) { return R32(c->esp + 4 + 4 * i); }      // stack argument i
inline void RET(Cpu* c, uint32_t eax, int nargs) { c->eax = eax; c->esp += 4 + 4 * nargs; }
void note_hle_call(Cpu* c, const char* name);       // watchdog bookkeeping
void hle_log(const char* fmt, ...);
void hle_unimplemented(const char* name, Cpu* c);   // logs once per name
uint32_t guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args);
void without_gil(const std::function<void()>& fn);   // host wait with the guest lock released

// --- logging -------------------------------------------------------------------------------
extern FILE* g_log;
bool log_enabled(const char* channel);

}  // namespace tje
