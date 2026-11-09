// Less common helpers used by lifted code: 80-bit floats, SSE conversions, MMX.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "cpu.h"

namespace tje {

// x87 extended (80-bit) <-> double, for fld/fstp tbyte.
inline double f80_to_double(const uint8_t* p) {
    uint64_t mant; uint16_t se;
    std::memcpy(&mant, p, 8); std::memcpy(&se, p + 8, 2);
    int exp = se & 0x7fff;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (exp == 0 && mant == 0) return sign * 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : sign * INFINITY;
    return sign * std::ldexp((double)mant, exp - 16383 - 63);
}
inline void double_to_f80(uint8_t* p, double v) {
    uint64_t mant = 0; uint16_t se = std::signbit(v) ? 0x8000 : 0;
    double a = std::fabs(v);
    if (std::isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (std::isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (a != 0.0) {
        int e; double f = std::frexp(a, &e);              // a = f * 2^e, f in [0.5, 1)
        mant = (uint64_t)std::ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    std::memcpy(p, &mant, 8); std::memcpy(p + 8, &se, 2);
}

// SSE conversions (MXCSR default: round to nearest; out of range -> 0x80000000).
inline int32_t cvt_trunc32(float v) {
    if (!(v > -2147483904.0f && v < 2147483648.0f)) return INT32_MIN;
    return (int32_t)v;
}
inline int32_t cvt_round32(float v) {
    float r = std::nearbyint(v);
    if (!(r >= -2147483648.0f && r < 2147483648.0f)) return INT32_MIN;
    return (int32_t)r;
}

inline Cpu::Xmm load_vec(const uint8_t* p, int size) {
    Cpu::Xmm v{}; std::memcpy(v.b, p, size); return v;
}
template <class T> inline uint64_t vec_q(const T& v) { uint64_t q; std::memcpy(&q, &v, 8); return q; }

// MMX operations on a 64-bit destination (the low half of the register union).
template <class D, class S> inline void mmx_packssdw(D* d, const S& s) {
    int32_t a[2], b[2]; std::memcpy(a, d, 8); std::memcpy(b, &s, 8);
    auto sat = [](int32_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); };
    int16_t r[4] = {sat(a[0]), sat(a[1]), sat(b[0]), sat(b[1])};
    std::memcpy(d, r, 8);
}
template <class D, class S> inline void mmx_packuswb(D* d, const S& s) {
    int16_t a[8]; std::memcpy(a, d, 8); std::memcpy(a + 4, &s, 8);
    uint8_t r[8];
    for (int i = 0; i < 8; i++) r[i] = (uint8_t)(a[i] < 0 ? 0 : a[i] > 255 ? 255 : a[i]);
    std::memcpy(d, r, 8);
}
template <class D, class S> inline void mmx_paddw(D* d, const S& s) {
    uint16_t a[4], b[4]; std::memcpy(a, d, 8); std::memcpy(b, &s, 8);
    for (int i = 0; i < 4; i++) a[i] = (uint16_t)(a[i] + b[i]);
    std::memcpy(d, a, 8);
}
template <class D> inline void mmx_psraw(D* d, int n) {
    int16_t a[4]; std::memcpy(a, d, 8);
    for (int i = 0; i < 4; i++) a[i] = (int16_t)(a[i] >> (n > 15 ? 15 : n));
    std::memcpy(d, a, 8);
}
template <class D, class S> inline void mmx_punpcklbw(D* d, const S& s) {
    uint8_t a[8], b[8], r[8]; std::memcpy(a, d, 8); std::memcpy(b, &s, 8);
    for (int i = 0; i < 4; i++) { r[2 * i] = a[i]; r[2 * i + 1] = b[i]; }
    std::memcpy(d, r, 8);
}
template <class D, class S> inline void mmx_pavgb(D* d, const S& s) {
    uint8_t a[8], b[8]; std::memcpy(a, d, 8); std::memcpy(b, &s, 8);
    for (int i = 0; i < 8; i++) a[i] = (uint8_t)((a[i] + b[i] + 1) >> 1);
    std::memcpy(d, a, 8);
}
template <class D, class S> inline void mmx_pavgw(D* d, const S& s) {
    uint16_t a[4], b[4]; std::memcpy(a, d, 8); std::memcpy(b, &s, 8);
    for (int i = 0; i < 4; i++) a[i] = (uint16_t)((a[i] + b[i] + 1) >> 1);
    std::memcpy(d, a, 8);
}
template <class D, class S> inline void mmx_pcmpgtd(D* d, const S& s) {
    int32_t a[2], b[2]; std::memcpy(a, d, 8); std::memcpy(b, &s, 8);
    for (int i = 0; i < 2; i++) a[i] = a[i] > b[i] ? -1 : 0;
    std::memcpy(d, a, 8);
}
template <class D, class S> inline void mmx_punpckhbw(D* d, const S& s) {
    uint8_t a[8], b[8], r[8]; std::memcpy(a, d, 8); std::memcpy(b, &s, 8);
    for (int i = 0; i < 4; i++) { r[2 * i] = a[4 + i]; r[2 * i + 1] = b[4 + i]; }
    std::memcpy(d, r, 8);
}

}  // namespace tje
