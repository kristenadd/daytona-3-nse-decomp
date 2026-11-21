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

# ---- recomp: guest memory, machine state, dispatch, helpers for translated code ----
add_library(dcusa_recomp STATIC
    src/recomp/guest_memory.cpp
    src/recomp/dispatch.cpp
    src/recomp/guest.cpp)
target_include_directories(dcusa_recomp PUBLIC src)
target_link_libraries(dcusa_recomp PUBLIC dcusa_runtime)
dcusa_warnings(dcusa_recomp)

# ---- translated game code ----
# tools/x86tr generates it from the player's prepared EXE image (dcprepare, then
# discover.py and translate.py). It lives in the build tree only; point
# DCUSA_GENERATED_DIR at a translate.py output folder to build it.
set(DCUSA_GENERATED_DIR "" CACHE PATH "translate.py output folder (holds sources.txt); empty to build without game code")
if(DCUSA_GENERATED_DIR)
    if(NOT EXISTS "${DCUSA_GENERATED_DIR}/sources.txt")
        message(FATAL_ERROR "DCUSA_GENERATED_DIR has no sources.txt: run tools/x86tr/translate.py first")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DCUSA_GENERATED_DIR}/sources.txt")
    file(STRINGS "${DCUSA_GENERATED_DIR}/sources.txt" dcusa_generated_names)
    list(TRANSFORM dcusa_generated_names PREPEND "${DCUSA_GENERATED_DIR}/")
    add_library(dcusa_generated STATIC ${dcusa_generated_names})
    target_link_libraries(dcusa_generated PUBLIC dcusa_recomp)
    # Same floating-point rules as the runtime; no warnings (the code mirrors the
    # guest instruction by instruction) and no debug info (it would run to gigabytes).
    target_compile_options(dcusa_generated PRIVATE -w -O2 -g0 -ffp-contract=off -fno-fast-math)
endif()

# ---- tools ----
add_executable(dcprepare tools/dcprepare/main.cpp)
target_link_libraries(dcprepare PRIVATE dcusa_system)
dcusa_warnings(dcprepare)

# ---- tests ----
enable_testing()
function(dcusa_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE dcusa_system dcusa_recomp)
    dcusa_warnings(${name})
    add_test(NAME ${name} COMMAND ${name} ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

dcusa_test(test_sha256)
dcusa_test(test_pe32)
dcusa_test(test_exe_image)
dcusa_test(test_content)
dcusa_test(test_fail_closed)

# Translator semantics against real x86: tools/x86tr/semtest.py runs every case
# natively as x86_64 under Rosetta 2 and translates the same snippet assembled
# for i386; the test checks the translation reproduces the x86 results.
find_package(Python3 COMPONENTS Interpreter)
set(DCUSA_PYTHONPATH "${CMAKE_CURRENT_SOURCE_DIR}/build/pylib" CACHE PATH
    "PYTHONPATH for tools/x86tr (where capstone is installed)")
if(APPLE AND Python3_FOUND)
    set(dcusa_semdir "${CMAKE_CURRENT_BINARY_DIR}/x86sem")
    file(GLOB dcusa_x86tr_scripts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/x86tr/*.py")
    add_custom_command(
        OUTPUT "${dcusa_semdir}/x86_semantics.cpp"
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${DCUSA_PYTHONPATH}"
                ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/x86tr/semtest.py" --out "${dcusa_semdir}"
        DEPENDS ${dcusa_x86tr_scripts}
        COMMENT "x86 semantics cases: ground truth under Rosetta, translation of the i386 encodings")
    add_executable(test_x86_semantics "${dcusa_semdir}/x86_semantics.cpp")
    target_link_libraries(test_x86_semantics PRIVATE dcusa_recomp)
    target_compile_options(test_x86_semantics PRIVATE -w -O1 -ffp-contract=off -fno-fast-math)
    add_test(NAME test_x86_semantics COMMAND test_x86_semantics)
endif()
# Needs the player's files (skips otherwise). Writes caches under the build tree only.
dcusa_test(test_game_files "${DCUSA_GAME_LIBRARY}" "${CMAKE_CURRENT_BINARY_DIR}/test-cache")
set_tests_properties(test_game_files PROPERTIES LABELS "game-files" TIMEOUT 300)
