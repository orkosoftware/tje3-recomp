// Runtime for code lifted from default.xbe by tools/recomp/lift.py.
//
// Guest state: eight 32-bit registers, the x87 stack (as doubles, matching the
// 53-bit precision control the game runs with), SSE/MMX registers and the
// direction flag. Arithmetic flags are not here: every lifted function keeps
// them in locals (see FLAGS_*), so the C++ compiler drops dead flag work and
// folds cmp + jcc pairs into plain comparisons.
//
// Guest memory is one flat arena: guest address A lives at g_mem + A.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace tje {

extern uint8_t* g_mem;

struct Cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;                 // direction flag (0 = up)
    uint32_t fs_base;            // guest address of the thread information block (fs:)
    double st[8];                // x87 registers, indexed physically
    uint32_t ftop;               // x87 stack top
    uint16_t fsw_cc;             // C0..C3 condition bits of the status word
    uint16_t fcw;                // control word (rounding control used by frndint/fist)
    union Xmm { float f[4]; uint32_t u[4]; uint64_t q[2]; int16_t w[8]; uint8_t b[16]; } xmm[8];
    union Mm { uint64_t q; uint32_t u[2]; int16_t w[4]; uint16_t uw[4]; uint8_t b[8]; } mm[8];
};

// --- memory ------------------------------------------------------------------
inline uint8_t  R8(uint32_t a)  { return g_mem[a]; }
inline uint16_t R16(uint32_t a) { uint16_t v; std::memcpy(&v, g_mem + a, 2); return v; }
inline uint32_t R32(uint32_t a) { uint32_t v; std::memcpy(&v, g_mem + a, 4); return v; }
inline uint64_t R64(uint32_t a) { uint64_t v; std::memcpy(&v, g_mem + a, 8); return v; }
inline float    RF32(uint32_t a) { float v; std::memcpy(&v, g_mem + a, 4); return v; }
inline double   RF64(uint32_t a) { double v; std::memcpy(&v, g_mem + a, 8); return v; }
inline void W8(uint32_t a, uint8_t v)   { g_mem[a] = v; }
inline void W16(uint32_t a, uint16_t v) { std::memcpy(g_mem + a, &v, 2); }
inline void W32(uint32_t a, uint32_t v) { std::memcpy(g_mem + a, &v, 4); }
inline void W64(uint32_t a, uint64_t v) { std::memcpy(g_mem + a, &v, 8); }
inline void WF32(uint32_t a, float v)   { std::memcpy(g_mem + a, &v, 4); }
inline void WF64(uint32_t a, double v)  { std::memcpy(g_mem + a, &v, 8); }

// --- sub-registers -------------------------------------------------------------
inline uint32_t set8lo(uint32_t r, uint32_t v)  { return (r & 0xffffff00u) | (v & 0xff); }
inline uint32_t set8hi(uint32_t r, uint32_t v)  { return (r & 0xffff00ffu) | ((v & 0xff) << 8); }
inline uint32_t set16(uint32_t r, uint32_t v)   { return (r & 0xffff0000u) | (v & 0xffff); }

// --- lazy flags ------------------------------------------------------------------
// Flag-setting instructions record (op, width, a, b, result); conditions are
// derived on use. EAGER ops store CF and OF directly.
enum FlagOp : int { F_ADD, F_SUB, F_LOGIC, F_INC, F_DEC, F_NEG, F_EAGER };

inline uint32_t wmask(int w) { return w == 32 ? 0xffffffffu : ((1u << w) - 1); }
inline uint32_t sbit(uint32_t v, int w) { return (v >> (w - 1)) & 1; }

inline bool flag_cf(int op, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t cf) {
    uint32_t m = wmask(w);
    switch (op) {
    case F_ADD: return (r & m) < (a & m);
    case F_SUB: return (a & m) < (b & m);
    case F_NEG: return (a & m) != 0;
    case F_LOGIC: return false;
    default: return cf != 0;           // INC, DEC, EAGER
    }
}
inline bool flag_of(int op, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t of) {
    switch (op) {
    case F_ADD: return sbit((a ^ r) & (b ^ r), w);
    case F_SUB: return sbit((a ^ b) & (a ^ r), w);
    case F_INC: return (r & wmask(w)) == (1u << (w - 1));
    case F_DEC: return (r & wmask(w)) == (1u << (w - 1)) - 1;
    case F_NEG: return (a & wmask(w)) == (1u << (w - 1));
    case F_LOGIC: return false;
    default: return of != 0;
    }
}
inline bool flag_zf(int w, uint32_t r) { return (r & wmask(w)) == 0; }
inline bool flag_sf(int w, uint32_t r) { return sbit(r, w) != 0; }
inline bool flag_pf(uint32_t r) {
    uint32_t v = r & 0xff;
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1) == 0;
}

// --- x87 -----------------------------------------------------------------------
inline double& ST(Cpu* c, int i) { return c->st[(c->ftop + i) & 7]; }
inline void fpush(Cpu* c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
inline void fpop(Cpu* c) { c->ftop = (c->ftop + 1) & 7; }
// Compare st0 with v, setting C0/C2/C3 like fcom (unordered -> all three).
inline void fcom(Cpu* c, double a, double b) {
    if (std::isnan(a) || std::isnan(b)) c->fsw_cc = 0x4500;
    else if (a < b) c->fsw_cc = 0x0100;
    else if (a == b) c->fsw_cc = 0x4000;
    else c->fsw_cc = 0;
}
inline uint16_t fnstsw(Cpu* c) { return (uint16_t)(c->fsw_cc | ((c->ftop & 7) << 11)); }
// Round to integer per the control word's rounding field.
inline double fround(Cpu* c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return std::nearbyint(v);   // host default: round to nearest even
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}
inline int32_t fist32(Cpu* c, double v) {
    double r = fround(c, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return INT32_MIN;   // integer indefinite
    return (int32_t)r;
}
inline int16_t fist16(Cpu* c, double v) {
    double r = fround(c, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return INT16_MIN;
    return (int16_t)r;
}
inline int64_t fist64(Cpu* c, double v) {
    double r = fround(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)r;
}

// --- call tracing (debug) ------------------------------------------------------------
extern bool g_trace;                                // any tracing enabled
void trace_enter(Cpu* c, uint32_t fn);
void trace_leave(Cpu* c, uint32_t fn);
#define TJE_ENTER(a) do { if (::tje::g_trace) ::tje::trace_enter(c, a); } while (0)
#define TJE_LEAVE(a) do { if (::tje::g_trace) ::tje::trace_leave(c, a); } while (0)

// --- preemption (see runtime.cpp) -----------------------------------------------------
extern volatile bool g_yield_request;
void yield_point(Cpu* c);
#define TJE_YIELD() do { if (::tje::g_yield_request) ::tje::yield_point(c); } while (0)

// --- dispatch --------------------------------------------------------------------
using GuestFn = void (*)(Cpu*);
struct LiftedEntry { uint32_t addr; GuestFn fn; };
extern const LiftedEntry kLifted[];            // generated table of lifted functions
extern const unsigned kLiftedCount;
void register_fn(uint32_t addr, GuestFn fn);   // add or override (HLE) an entry
GuestFn lookup(uint32_t addr);                     // lifted function or HLE override
[[noreturn]] void fault(const char* what, uint32_t addr);
void call_indirect(Cpu* c, uint32_t target);       // lookup + call, faults when unknown

}  // namespace tje

namespace tje {
// rdtsc: real time counted at the Xbox CPU clock (733.33 MHz), which is what
// QueryPerformanceFrequency reports.
uint64_t guest_rdtsc();
}  // namespace tje
