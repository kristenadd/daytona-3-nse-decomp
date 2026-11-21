cmake_minimum_required(VERSION 3.24)
project(DaytonaUSA LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "Apple builds require -DCMAKE_OSX_ARCHITECTURES=arm64 (use the presets)")
endif()

# The player's own installation. Read only: tools and tests never write there,
# and nothing from it is copied into the source tree.
set(DCUSA_DEFAULT_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/../Daytona Championship USA")
set(DCUSA_GAME_LIBRARY "${DCUSA_DEFAULT_LIBRARY}" CACHE PATH
    "Folder holding the player's Daytona Championship USA installation(s); tests that need it skip when absent")

find_package(ZLIB REQUIRED)

function(dcusa_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /fp:strict)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
                                                 -ffp-contract=off -fno-fast-math)
    endif()
endfunction()

# ---- system: hashing, PE mapping, the player's EXE image, content discovery ----
add_library(dcusa_system STATIC
    src/system/sha256.cpp
    src/system/pe32.cpp
    src/system/edition.cpp
    src/system/exe_manifest.cpp
    src/system/exe_image.cpp
    src/system/content.cpp
    src/system/files.cpp)
target_include_directories(dcusa_system PUBLIC src)
target_link_libraries(dcusa_system PUBLIC ZLIB::ZLIB)
dcusa_warnings(dcusa_system)

# ---- runtime: fail-closed reporting and the platform contract ----
add_library(dcusa_runtime STATIC
    src/runtime/fail_closed.cpp)
target_include_directories(dcusa_runtime PUBLIC src)
dcusa_warnings(dcusa_runtime)

# ---- tools ----
add_executable(dcprepare tools/dcprepare/main.cpp)
target_link_libraries(dcprepare PRIVATE dcusa_system)
dcusa_warnings(dcprepare)

# ---- tests ----
enable_testing()
function(dcusa_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE dcusa_system dcusa_runtime)
    dcusa_warnings(${name})
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

dcusa_test(test_sha256)
dcusa_test(test_pe32)
dcusa_test(test_exe_image)
dcusa_test(test_content)
dcusa_test(test_fail_closed)
# Needs the player's files (skips otherwise). Writes caches under the build tree only.
dcusa_test(test_game_files "${DCUSA_GAME_LIBRARY}" "${CMAKE_CURRENT_BINARY_DIR}/test-cache")
set_tests_properties(test_game_files PROPERTIES LABELS "game-files" TIMEOUT 300)
