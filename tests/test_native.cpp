// The native cdecl replacements: arguments sit above the return address,
// eax comes back, and the callee-saved registers stay put.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "check.hpp"
#include "recomp/guest_memory.hpp"
#include "recomp/native.hpp"
#include "recomp/x86ops.hpp"

using namespace dcusa::recomp;

namespace {

constexpr std::uint32_t Stack = 0x210000;
constexpr std::uint32_t Buf = 0x200000;

Cpu make_cpu(GuestMemory& mem) {
    Cpu c;
    c.mem = mem.base();
    c.r[Cpu::ESI] = 0x11111111u;
    c.r[Cpu::EDI] = 0x22222222u;
    c.r[Cpu::EBX] = 0x33333333u;
    c.r[Cpu::EBP] = 0x44444444u;
    c.df = 1;
    return c;
}

void push_args(Cpu& c, std::initializer_list<std::uint32_t> args) {
    st32(c.mem, Stack, 0);
    unsigned i = 0;
    for (std::uint32_t a : args) st32(c.mem, Stack + 4u + 4u * i++, a);
    c.r[Cpu::ESP] = Stack;
}

void expect_frame(const Cpu& c) {
    CHECK(c.r[Cpu::ESP] == Stack + 4u);
    CHECK(c.r[Cpu::ESI] == 0x11111111u);
    CHECK(c.r[Cpu::EDI] == 0x22222222u);
    CHECK(c.r[Cpu::EBX] == 0x33333333u);
    CHECK(c.r[Cpu::EBP] == 0x44444444u);
}

void put_str(std::uint8_t* m, std::uint32_t at, const char* s) {
    std::memcpy(m + at, s, std::strlen(s) + 1);
}

}  // namespace

int main() {
    GuestMemory mem;
    std::string error;
    REQUIRE(mem.reserve(&error));
    REQUIRE(mem.commit(Buf, 0x2000, &error));
    REQUIRE(mem.commit(Stack, 0x1000, &error));
    std::uint8_t* m = mem.base();

    {
        Cpu c = make_cpu(mem);
        std::memcpy(m + Buf, "abcdefghij", 10);
        push_args(c, {Buf + 2, Buf, 8});
        native_memmove(c);
        expect_frame(c);
        CHECK(c.r[Cpu::EAX] == Buf + 2);
        CHECK(c.df == 0);
        CHECK(std::memcmp(m + Buf + 2, "abcdefgh", 8) == 0);
    }
    {
        Cpu c = make_cpu(mem);
        push_args(c, {Buf + 16, 0xAB, 4});
        native_memset(c);
        expect_frame(c);
        CHECK(c.r[Cpu::EAX] == Buf + 16);
        CHECK(m[Buf + 16] == 0xAB && m[Buf + 19] == 0xAB);
        CHECK(c.df == 1);
    }
    {
        Cpu c = make_cpu(mem);
        put_str(m, Buf, "ababa");
        push_args(c, {Buf, std::uint32_t('a')});
        native_strrchr(c);
        expect_frame(c);
        CHECK(c.r[Cpu::EAX] == Buf + 4);
        CHECK(c.df == 0);
        push_args(c, {Buf, 0});
        c.r[Cpu::ESI] = 0x11111111u;
        native_strrchr(c);
        CHECK(c.r[Cpu::EAX] == Buf + 5);
    }
    {
        Cpu c = make_cpu(mem);
        put_str(m, Buf, "abc");
        push_args(c, {Buf, std::uint32_t('b')});
        native_strchr(c);
        expect_frame(c);
        CHECK(c.r[Cpu::EAX] == Buf + 1);
        CHECK(c.df == 1);
        push_args(c, {Buf, std::uint32_t('z')});
        native_strchr(c);
        CHECK(c.r[Cpu::EAX] == 0);
        push_args(c, {Buf, 0});
        native_strchr(c);
        CHECK(c.r[Cpu::EAX] == Buf + 3);
    }
    {
        Cpu c = make_cpu(mem);
        put_str(m, Buf, "hello");
        put_str(m, Buf + 16, "ll");
        push_args(c, {Buf, Buf + 16});
        native_strstr(c);
        expect_frame(c);
        CHECK(c.r[Cpu::EAX] == Buf + 2);
        put_str(m, Buf + 16, "");
        push_args(c, {Buf, Buf + 16});
        native_strstr(c);
        CHECK(c.r[Cpu::EAX] == Buf);
        put_str(m, Buf + 16, "zz");
        push_args(c, {Buf, Buf + 16});
        native_strstr(c);
        CHECK(c.r[Cpu::EAX] == 0);
    }
    return check_result();
}
