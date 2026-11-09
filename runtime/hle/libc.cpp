// Native replacements for C runtime functions that do not lift cleanly
// (hand-written assembly with computed jumps).
#include <cstring>

#include "guest.h"

namespace tje {

// memcpy / memmove (the CRT shares one implementation; overlap-safe). cdecl (dst, src, n) -> dst
void hle_0012a8e0(Cpu* c) {
    uint32_t dst = A(c, 0), src = A(c, 1), n = A(c, 2);
    std::memmove(g_mem + dst, g_mem + src, n);
    RET(c, dst, 0);
}

}  // namespace tje
