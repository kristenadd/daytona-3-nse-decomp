# Cross-compiling for x86_64 Linux (the Steam Deck) from a Mac: Homebrew's
# clang and lld against the Steam Runtime 3 ("sniper") SDK sysroot, so the
# binary runs on SteamOS and inside Steam's Linux runtime.
#
#   brew install llvm lld
#   sysroot: com.valvesoftware.SteamRuntime.Sdk-amd64,i386-sniper-sysroot.tar.gz
#   from repo.steampowered.com/steamrt-images-sniper/snapshots/latest-public-stable/,
#   unpacked to build/toolchains/sniper (or DCUSA_LINUX_SYSROOT).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT DCUSA_LINUX_SYSROOT)
    set(DCUSA_LINUX_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../build/toolchains/sniper")
endif()
get_filename_component(DCUSA_LINUX_SYSROOT "${DCUSA_LINUX_SYSROOT}" ABSOLUTE)
set(CMAKE_SYSROOT "${DCUSA_LINUX_SYSROOT}")
if(NOT DCUSA_LLVM)
    set(DCUSA_LLVM /opt/homebrew/opt/llvm)
endif()
set(CMAKE_C_COMPILER "${DCUSA_LLVM}/bin/clang")
set(CMAKE_CXX_COMPILER "${DCUSA_LLVM}/bin/clang++")
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_AR "${DCUSA_LLVM}/bin/llvm-ar")
set(CMAKE_RANLIB "${DCUSA_LLVM}/bin/llvm-ranlib")
# The Steam Deck's CPU (Zen 2): AVX2, tuned for it.
set(CMAKE_C_FLAGS_INIT "-march=x86-64-v3 -mtune=znver2")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64-v3 -mtune=znver2")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=/opt/homebrew/opt/lld/bin/ld.lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=/opt/homebrew/opt/lld/bin/ld.lld")
set(CMAKE_FIND_ROOT_PATH "${DCUSA_LINUX_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
