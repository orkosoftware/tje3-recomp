// Runs lifted functions on recorded memory snapshots (tools/recomp/test_lifted.py).
//
//   harness <default.xbe> <cases.bin> <results.bin>
//
// cases.bin:   u32 'TJEC', u32 n, then per case:
//                u32 func, ecx, edx, esp, heap_top, npages, {u32 addr, u8[4096]} * npages,
//                u32 nout, u32 out_addr * nout
// results.bin: per case: u32 status (0 ok, 1 fault), u32 eax, u8[4096] * nout
//
// Memory between cases: the XBE image is reloaded, the stack / heap / fake level
// regions are cleared, then the case's pages are applied.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <setjmp.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "cpu.h"

namespace tje {
uint8_t* g_mem = nullptr;

static std::map<uint32_t, GuestFn>& registry() {
    static std::map<uint32_t, GuestFn> r;
    return r;
}
void register_fn(uint32_t addr, GuestFn fn) { registry()[addr] = fn; }

GuestFn lookup(uint32_t addr) {
    auto& r = registry();
    auto it = r.find(addr);
    return it == r.end() ? nullptr : it->second;
}

static jmp_buf g_fault_jmp;
static char g_fault_msg[256];

[[noreturn]] void fault(const char* what, uint32_t addr) {
    std::snprintf(g_fault_msg, sizeof g_fault_msg, "%s at %08x", what, addr);
    longjmp(g_fault_jmp, 1);
}

// Runtime hooks the lifted code expects (no tracing, no threads here).
bool g_trace = false;
void trace_enter(Cpu*, uint32_t) {}
void trace_leave(Cpu*, uint32_t) {}
volatile bool g_yield_request = false;
void yield_point(Cpu*) {}
uint64_t guest_rdtsc() { return 0; }

void call_indirect(Cpu* c, uint32_t target) {
    GuestFn f = lookup(target);
    if (!f) fault("unknown indirect target", target);
    f(c);
}
}  // namespace tje

using namespace tje;

static const uint32_t kArenaSize = 0x11000000;   // image .. fake level .. stack .. heap
static uint32_t g_case_func;

#ifdef _WIN32
// The arena reserves the whole 4 GB guest space; pages beyond the regions the
// harness commits up front are committed on first touch (and reported, since
// the original never touched them in the recorded runs).
static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* a = (uint8_t*)e->ExceptionRecord->ExceptionInformation[1];
    if (a < g_mem || a >= g_mem + 0x100000000ull) return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr, "  function %08x touched guest address %08x\n", g_case_func, (uint32_t)(a - g_mem));
    if (!VirtualAlloc((void*)((uintptr_t)a & ~(uintptr_t)0xfff), 0x1000, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

static std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> d;
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
    std::fseek(f, 0, SEEK_END);
    d.resize(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (!d.empty() && std::fread(d.data(), 1, d.size(), f) != d.size()) std::exit(2);
    std::fclose(f);
    return d;
}

struct Section { uint32_t va, vsize, raw, rsize; };

static std::vector<Section> xbe_sections(const std::vector<uint8_t>& x) {
    auto u32 = [&](uint32_t o) { uint32_t v; std::memcpy(&v, &x[o], 4); return v; };
    uint32_t base = u32(0x104), n = u32(0x11c), hdr = u32(0x120) - base;
    std::vector<Section> s;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h = hdr + i * 0x38;
        s.push_back({u32(h + 4), u32(h + 8), u32(h + 12), u32(h + 16)});
    }
    return s;
}

// Provided by the generated HLE file: the test stubs and the allocator's heap top.
void register_hle();
namespace tje { extern uint32_t g_heap_top; }

int main(int argc, char** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: harness <xbe> <cases> <results>\n"); return 2; }
#ifdef _WIN32
    g_mem = (uint8_t*)VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_READWRITE);
    if (g_mem && !VirtualAlloc(g_mem, kArenaSize, MEM_COMMIT, PAGE_READWRITE)) g_mem = nullptr;
    AddVectoredExceptionHandler(1, commit_on_touch);
#else
    g_mem = (uint8_t*)std::calloc(kArenaSize, 1);
#endif
    if (!g_mem) { std::fprintf(stderr, "arena allocation failed\n"); return 2; }
    for (unsigned i = 0; i < kLiftedCount; i++) register_fn(kLifted[i].addr, kLifted[i].fn);
    register_hle();

    std::vector<uint8_t> xbe = read_file(argv[1]);
    std::vector<Section> secs = xbe_sections(xbe);
    std::vector<uint8_t> cases = read_file(argv[2]);
    FILE* out = std::fopen(argv[3], "wb");
    size_t p = 0;
    auto rd = [&]() { uint32_t v; std::memcpy(&v, &cases[p], 4); p += 4; return v; };
    if (rd() != 0x43454a54) { std::fprintf(stderr, "bad case file\n"); return 2; }
    uint32_t n = rd();
    for (uint32_t k = 0; k < n; k++) {
        uint32_t func = rd(), ecx = rd(), edx = rd(), esp = rd(), heap_top = rd(), npages = rd();
        // reset memory
        std::memset(g_mem, 0, 0x400000);
        for (auto& s : secs)
            std::memcpy(g_mem + s.va, &xbe[s.raw], s.rsize < s.vsize ? s.rsize : s.vsize);
        std::memset(g_mem + 0x0e000000, 0, 0x100000);
        std::memset(g_mem + 0x0ef00000, 0, 0x100000);
        std::memset(g_mem + 0x10000000, 0, (heap_top - 0x10000000) + 0x100000);
        for (uint32_t i = 0; i < npages; i++) {
            uint32_t a = rd();
            std::memcpy(g_mem + a, &cases[p], 4096);
            p += 4096;
        }
        uint32_t nout = rd();
        std::vector<uint32_t> outs(nout);
        for (auto& o : outs) o = rd();

        tje::g_heap_top = heap_top;
        Cpu cpu{};
        cpu.ecx = ecx; cpu.edx = edx; cpu.esp = esp; cpu.fcw = 0x027f;
        uint32_t status = 0;
        g_case_func = func;
        GuestFn f = lookup(func);
        if (!f) { std::snprintf(g_fault_msg, sizeof g_fault_msg, "function %08x not lifted", func); status = 1; }
        else if (setjmp(g_fault_jmp) == 0) f(&cpu);
        else status = 1;
        if (status) std::fprintf(stderr, "case %u (%08x): %s\n", k, func, g_fault_msg);
        std::fwrite(&status, 4, 1, out);
        std::fwrite(&cpu.eax, 4, 1, out);
        for (uint32_t o : outs) std::fwrite(g_mem + o, 1, 4096, out);
    }
    std::fclose(out);
    return 0;
}
