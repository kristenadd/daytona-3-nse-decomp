#include "recomp/native.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "recomp/x86ops.hpp"
#include "runtime/fail_closed.hpp"

namespace dcusa::recomp {
namespace {

std::uint32_t arg(const Cpu& c, unsigned i) { return ld32(c.mem, c.r[Cpu::ESP] + 4u + 4u * i); }

void ret_cdecl(Cpu& c, std::uint32_t eax) {
    c.r[Cpu::EAX] = eax;
    c.r[Cpu::ESP] += 4;
}

}  // namespace

void native_memmove(Cpu& c) {
    const std::uint32_t dst = arg(c, 0);
    const std::uint32_t src = arg(c, 1);
    const std::uint32_t n = arg(c, 2);
    if (n != 0 && (dst < 0x10000u || src < 0x10000u)) {
        char what[96];
        std::snprintf(what, sizeof what, "memmove(%08X, %08X, %u) called from %08X", dst, src, n, ld32(c.mem, c.r[Cpu::ESP]));
        fail_closed(Unported::Function, ld32(c.mem, c.r[Cpu::ESP]), what);
    }
    if (n != 0) std::memmove(c.mem + dst, c.mem + src, n);
    // The CRT's backward copy sets DF and clears it before returning.
    c.df = 0;
    ret_cdecl(c, dst);
}

void native_memset(Cpu& c) {
    const std::uint32_t dst = arg(c, 0);
    const int fill = static_cast<int>(static_cast<std::uint8_t>(arg(c, 1)));
    const std::uint32_t n = arg(c, 2);
    if (n != 0) std::memset(c.mem + dst, fill, n);
    ret_cdecl(c, dst);
}

void native_strchr(Cpu& c) {
    const std::uint8_t ch = static_cast<std::uint8_t>(arg(c, 1));
    std::uint32_t p = arg(c, 0);
    for (;;) {
        const std::uint8_t b = c.mem[p];
        if (b == ch) {
            ret_cdecl(c, p);
            return;
        }
        if (b == 0) {
            ret_cdecl(c, 0);
            return;
        }
        ++p;
    }
}

void native_strrchr(Cpu& c) {
    const std::uint8_t ch = static_cast<std::uint8_t>(arg(c, 1));
    std::uint32_t p = arg(c, 0);
    std::uint32_t last = 0;
    for (;;) {
        const std::uint8_t b = c.mem[p];
        if (b == ch) last = p;
        if (b == 0) break;
        ++p;
    }
    c.df = 0;
    ret_cdecl(c, ch == 0 ? p : last);
}

void native_crt_unary(Cpu& c) {
    const std::uint32_t desc = c.r[Cpu::EDX];
    char name[16] = {};
    std::uint32_t p = desc;
    if (c.mem[p] < 'a' || c.mem[p] > 'z') ++p;
    for (std::uint32_t i = 0; i + 1 < sizeof name; ++i, ++p) {
        const unsigned char ch = c.mem[p];
        if (ch < 'a' || ch > 'z') break;
        name[i] = static_cast<char>(ch);
    }
    const double x = c.fpu.at(0);
    double y = 0;
    if (std::strcmp(name, "exp") == 0) y = std::exp(x);
    else if (std::strcmp(name, "log") == 0) y = std::log(x);
    else if (std::strcmp(name, "log10") == 0) y = std::log10(x);
    else if (std::strcmp(name, "sin") == 0) y = std::sin(x);
    else if (std::strcmp(name, "cos") == 0) y = std::cos(x);
    else if (std::strcmp(name, "tan") == 0) y = std::tan(x);
    else if (std::strcmp(name, "asin") == 0) y = std::asin(x);
    else if (std::strcmp(name, "acos") == 0) y = std::acos(x);
    else if (std::strcmp(name, "atan") == 0) y = std::atan(x);
    else if (std::strcmp(name, "sqrt") == 0) y = std::sqrt(x);
    else if (std::strcmp(name, "fabs") == 0) y = std::fabs(x);
    else if (std::strcmp(name, "sinh") == 0) y = std::sinh(x);
    else if (std::strcmp(name, "cosh") == 0) y = std::cosh(x);
    else if (std::strcmp(name, "tanh") == 0) y = std::tanh(x);
    else if (std::strcmp(name, "floor") == 0) y = std::floor(x);
    else if (std::strcmp(name, "ceil") == 0) y = std::ceil(x);
    else {
        char buf[48];
        std::snprintf(buf, sizeof buf, "crt math %s", name[0] ? name : "?");
        fail_closed(Unported::Function, desc, buf);
    }
    c.fpu.at(0) = y;
}

void native_strstr(Cpu& c) {
    const std::uint32_t hay = arg(c, 0);
    const std::uint32_t needle = arg(c, 1);
    if (c.mem[needle] == 0) {
        ret_cdecl(c, hay);
        return;
    }
    for (std::uint32_t i = hay;; ++i) {
        if (c.mem[i] == 0) {
            ret_cdecl(c, 0);
            return;
        }
        std::uint32_t a = i;
        std::uint32_t b = needle;
        while (c.mem[b] != 0 && c.mem[a] == c.mem[b]) {
            ++a;
            ++b;
        }
        if (c.mem[b] == 0) {
            ret_cdecl(c, i);
            return;
        }
    }
}

}  // namespace dcusa::recomp
