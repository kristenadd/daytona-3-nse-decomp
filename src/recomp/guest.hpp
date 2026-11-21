#pragma once
// Included by every generated translation shard.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "recomp/cpu.hpp"
#include "recomp/dispatch.hpp"
#include "recomp/x86ops.hpp"

namespace dcusa::recomp {

// A guest fault (int3, ud2, divide error, a call that should never return):
// SEH is not emulated for these, so the game stops with a report.
[[noreturn]] void trap(Cpu& c, std::uint32_t va, const char* what);
// A guest function the translator could not translate, or did not select.
[[noreturn]] void guest_unported(Cpu& c, std::uint32_t va, const char* why);
void guest_cpuid(Cpu& c);
std::uint64_t guest_rdtsc();

template <class T, int N, class V, class Op> inline void lanes(V& d, const V& s, Op op) {
    T* dp = reinterpret_cast<T*>(&d);
    const T* sp = reinterpret_cast<const T*>(&s);
    for (int i = 0; i < N; ++i) {
        T a, b;
        std::memcpy(&a, dp + i, sizeof(T));
        std::memcpy(&b, sp + i, sizeof(T));
        T r = op(a, b);
        std::memcpy(dp + i, &r, sizeof(T));
    }
}

// NaN results the x86 way. An invalid operation on non-NaN inputs gives the
// "default NaN", which is negative on x86 (FFC00000) and positive on ARM; with
// NaN inputs x86 returns the first NaN operand (quieted) where ARM prefers a
// signalling NaN wherever it is.
template <class T> inline T x86_quiet(T v) {
    if constexpr (sizeof(T) == 4) return std::bit_cast<T>(std::bit_cast<std::uint32_t>(v) | 0x00400000u);
    else return std::bit_cast<T>(std::bit_cast<std::uint64_t>(v) | 0x0008000000000000ull);
}
template <class T> inline T x86_default_nan() {
    if constexpr (sizeof(T) == 4) return std::bit_cast<T>(0xFFC00000u);
    else return std::bit_cast<T>(0xFFF8000000000000ull);
}
template <class T> inline T x86_nan(T r, T a, T b) {
    if (r == r) return r;
    if (a != a) return x86_quiet(a);
    if (b != b) return x86_quiet(b);
    return x86_default_nan<T>();
}
template <class T> inline T x86_nan(T r, T a) {
    if (r == r) return r;
    if (a != a) return x86_quiet(a);
    return x86_default_nan<T>();
}

inline Xmm xmm_load(const std::uint8_t* M, std::uint32_t a, unsigned bytes) {
    Xmm v{};
    std::memcpy(&v, M + a, bytes);
    return v;
}
inline Mmx mmx_load(const std::uint8_t* M, std::uint32_t a, unsigned bytes) {
    Mmx v{};
    std::memcpy(&v, M + a, bytes);
    return v;
}
inline Xmm xmm_from(const Mmx& m) {
    Xmm v{};
    std::memcpy(&v, &m, 8);
    return v;
}

// 80-bit extended values in guest memory (fld / fstp tbyte), converted to and from double.
inline double ld80(const std::uint8_t* M, std::uint32_t a) {
    std::uint64_t mant;
    std::uint16_t se;
    std::memcpy(&mant, M + a, 8);
    std::memcpy(&se, M + a + 8, 2);
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0x7FFF) v = (mant << 1) ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else if (mant == 0) v = 0.0;
    else v = std::ldexp(double(mant), exp - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
inline void st80(std::uint8_t* M, std::uint32_t a, double v) {
    std::uint64_t mant = 0;
    std::uint16_t se = std::signbit(v) ? 0x8000 : 0;
    if (std::isnan(v)) { mant = 0xC000000000000000ull; se |= 0x7FFF; }
    else if (std::isinf(v)) { mant = 0x8000000000000000ull; se |= 0x7FFF; }
    else if (v != 0.0) {
        int e;
        double f = std::frexp(std::fabs(v), &e);
        mant = std::uint64_t(std::ldexp(f, 64));
        se |= std::uint16_t(e - 1 + 16383);
    }
    std::memcpy(M + a, &mant, 8);
    std::memcpy(M + a + 8, &se, 2);
}
// fxam: class of ST(0) into C3 / C2 / C0, its sign into C1.
inline void fpu_examine(Fpu& f) {
    double v = f.at(0);
    std::uint16_t s = std::uint16_t(f.status & ~0x4700u);
    if (std::signbit(v)) s |= 0x0200;
    if (((f.tag >> (2 * f.top)) & 3) == 3) s |= 0x4100;
    else if (std::isnan(v)) s |= 0x0100;
    else if (std::isinf(v)) s |= 0x0500;
    else if (v == 0.0) s |= 0x4000;
    else if (std::fpclassify(v) == FP_SUBNORMAL) s |= 0x4400;
    else s |= 0x0400;
    f.status = s;
}
inline double fpu_scale(double x, double y) {
    double n = std::trunc(y);
    if (n != n) return n;
    int e = n > 100000.0 ? 100000 : n < -100000.0 ? -100000 : int(n);
    return std::ldexp(x, e);
}
// fprem / fprem1: one complete reduction (C2 = 0), low quotient bits in C0 / C3 / C1.
inline void fpu_prem(Fpu& f, bool ieee) {
    double a = f.at(0), b = f.at(1);
    double q = ieee ? std::nearbyint(a / b) : std::trunc(a / b);
    double r = ieee ? std::remainder(a, b) : std::fmod(a, b);
    unsigned qi = std::isfinite(q) ? unsigned(std::fmod(std::fabs(q), 8.0)) : 0;
    std::uint16_t s = std::uint16_t(f.status & ~0x4700u);
    if (qi & 4) s |= 0x0100;
    if (qi & 2) s |= 0x4000;
    if (qi & 1) s |= 0x0200;
    f.status = s;
    f.at(0) = r;
}
inline std::int16_t fpu_trunc_i16(double v) {
    double r = std::trunc(v);
    if (!(r >= -32768.0 && r <= 32767.0)) return std::numeric_limits<std::int16_t>::min();
    return std::int16_t(r);
}

inline float f32_bits(std::uint32_t v) { return std::bit_cast<float>(v); }
inline std::uint32_t bits_f32(float v) { return std::bit_cast<std::uint32_t>(v); }
inline double f64_bits(std::uint64_t v) { return std::bit_cast<double>(v); }
inline std::uint64_t bits_f64(double v) { return std::bit_cast<std::uint64_t>(v); }

}

using namespace dcusa::recomp;

// Registers live in locals for the length of a translated function and are
// written back before anything that can observe them (calls, returns, traps).
#define PROLOGUE()                                                                                   \
    std::uint8_t* const M = c.mem;                                                                   \
    std::uint32_t eax = c.r[0], ecx = c.r[1], edx = c.r[2], ebx = c.r[3], esp = c.r[4], ebp = c.r[5], \
                  esi = c.r[6], edi = c.r[7];                                                        \
    std::uint8_t cf = c.cf, pf = c.pf, af = c.af, zf = c.zf, sf = c.sf, of = c.of;                   \
    std::uint32_t ta = 0, tb = 0;                                                                    \
    (void)M; (void)ta; (void)tb
#define SYNC()                                                                                       \
    (c.r[0] = eax, c.r[1] = ecx, c.r[2] = edx, c.r[3] = ebx, c.r[4] = esp, c.r[5] = ebp, c.r[6] = esi, \
     c.r[7] = edi, c.cf = cf, c.pf = pf, c.af = af, c.zf = zf, c.sf = sf, c.of = of)
#define RELOAD()                                                                                     \
    (eax = c.r[0], ecx = c.r[1], edx = c.r[2], ebx = c.r[3], esp = c.r[4], ebp = c.r[5], esi = c.r[6], \
     edi = c.r[7], cf = c.cf, pf = c.pf, af = c.af, zf = c.zf, sf = c.sf, of = c.of)
