#pragma once
// Native replacements for guest functions the translator would otherwise emit.
// Each one is a cdecl callee: arguments are dwords at [esp+4], [esp+8], ...,
// the return value is eax, and esp moves past the return address only.
// ebx, ebp, esi and edi are left alone.
#include "recomp/cpu.hpp"

namespace dcusa::recomp {

void native_memmove(Cpu& c);  // void* (dst, src, n) -> dst, overlap is defined
void native_memset(Cpu& c);   // void* (dst, fill, n) -> dst
void native_strchr(Cpu& c);   // char* (s, char) -> match or 0
void native_strrchr(Cpu& c);  // char* (s, char) -> last match or 0
void native_strstr(Cpu& c);   // char* (haystack, needle) -> match or 0; empty needle returns haystack
// The CRT unary dispatcher at 0xC44FD0 jumps through a descriptor whose name is the
// function ("exp", "log", ...). The handlers it selects are not translated. The argument
// is st(0); the result replaces it. The guest return address stays where it is.
void native_crt_unary(Cpu& c);

}
