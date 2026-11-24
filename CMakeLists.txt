cmake_minimum_required(VERSION 3.24)
project(DaytonaUSA LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# iOS / iPadOS (the ios-sim preset): the same libraries and an app instead of
# the Mac tools and tests (src/platform/ios).
set(DCUSA_IOS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(DCUSA_IOS ON)
endif()

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
    src/runtime/fail_closed.cpp
    src/runtime/pad_mapping.cpp
    src/runtime/pad_script.cpp)
target_include_directories(dcusa_runtime PUBLIC src)
dcusa_warnings(dcusa_runtime)

# ---- recomp: guest memory, machine state, dispatch, helpers for translated code ----
add_library(dcusa_recomp STATIC
    src/recomp/guest_memory.cpp
    src/recomp/dispatch.cpp
    src/recomp/guest.cpp
    src/recomp/loader.cpp
    src/recomp/native.cpp
    src/recomp/win32.cpp
    src/recomp/threads.cpp
    src/recomp/shell.cpp
    src/recomp/host_files.cpp
    src/recomp/d3d11.cpp
    src/recomp/avi_video.cpp
    src/audio/ds_mixer.cpp
    src/audio/null_output.cpp
    src/gpu/metal.mm
    src/gpu/bc_decode.cpp
    src/gpu/gpu_queue.mm)
target_include_directories(dcusa_recomp PUBLIC src)
target_link_libraries(dcusa_recomp PUBLIC dcusa_runtime "-framework Foundation" "-framework Metal" "-framework QuartzCore"
                      "-framework CoreGraphics")
set_source_files_properties(src/gpu/metal.mm src/gpu/gpu_queue.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
# d3d11.cpp runs tools/shaderprep on a shader it has no translation for (DCUSA_TRANSLATE=1).
set_source_files_properties(src/recomp/d3d11.cpp PROPERTIES COMPILE_DEFINITIONS "DCUSA_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
dcusa_warnings(dcusa_recomp)

# ---- translated game code ----
# tools/x86tr generates it from the player's prepared EXE image. Set
# DCUSA_PREPARED_CACHE and the build runs discovery and translation itself.
# DCUSA_GENERATED_DIR remains for a folder that was translated by hand.
set(DCUSA_PREPARED_CACHE "" CACHE FILEPATH
    "dcprepare cache. When set, discovery and translation run as part of the build")
set(DCUSA_GENERATED_DIR "" CACHE PATH
    "translate.py output folder (holds sources.txt); empty to build without game code")
set(DCUSA_PYTHONPATH "${CMAKE_CURRENT_SOURCE_DIR}/build/pylib" CACHE PATH
    "PYTHONPATH for tools/x86tr (where capstone is installed)")

set(dcusa_generated_dir "")
if(DCUSA_PREPARED_CACHE)
    if(NOT EXISTS "${DCUSA_PREPARED_CACHE}")
        message(FATAL_ERROR "DCUSA_PREPARED_CACHE does not exist: ${DCUSA_PREPARED_CACHE}")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Cache header: 8-byte magic, then five little-endian u32s. Edition is the second.
    file(READ "${DCUSA_PREPARED_CACHE}" dcusa_hdr LIMIT 16 HEX)
    string(TOLOWER "${dcusa_hdr}" dcusa_hdr)
    string(SUBSTRING "${dcusa_hdr}" 0 16 dcusa_magic)
    if(NOT dcusa_magic STREQUAL "4443555341494d47")
        message(FATAL_ERROR "DCUSA_PREPARED_CACHE is not a prepared image: ${DCUSA_PREPARED_CACHE}")
    endif()
    string(SUBSTRING "${dcusa_hdr}" 24 2 dcusa_e0)
    string(SUBSTRING "${dcusa_hdr}" 26 2 dcusa_e1)
    string(SUBSTRING "${dcusa_hdr}" 28 2 dcusa_e2)
    string(SUBSTRING "${dcusa_hdr}" 30 2 dcusa_e3)
    math(EXPR dcusa_edition "0x${dcusa_e0} + 0x${dcusa_e1} * 256 + 0x${dcusa_e2} * 65536 + 0x${dcusa_e3} * 16777216")
    if(NOT dcusa_edition EQUAL 2017 AND NOT dcusa_edition EQUAL 2019)
        message(FATAL_ERROR "prepared image edition ${dcusa_edition} is not 2017 or 2019")
    endif()
    set(dcusa_x86tr_dir "${CMAKE_SOURCE_DIR}/build/x86tr/${dcusa_edition}")
    set(dcusa_generated_dir "${dcusa_x86tr_dir}/gen")
    file(GLOB dcusa_x86tr_scripts CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/x86tr/*.py")
    # The stamp lives in the build tree. The generated shards keep their own mtimes, so
    # they are not the custom command's outputs: an unchanged shard would look stale.
    set(dcusa_stamp "${CMAKE_CURRENT_BINARY_DIR}/x86tr-translated.stamp")
    set(dcusa_translate_script "${CMAKE_SOURCE_DIR}/cmake/translate.cmake")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
                -DDCUSA_CACHE=${DCUSA_PREPARED_CACHE}
                -DDCUSA_X86TR_DIR=${dcusa_x86tr_dir}
                -DDCUSA_GEN_DIR=${dcusa_generated_dir}
                -DDCUSA_PYTHON=${Python3_EXECUTABLE}
                -DDCUSA_PYTHONPATH=${DCUSA_PYTHONPATH}
                -DDCUSA_SOURCE=${CMAKE_SOURCE_DIR}
                -DDCUSA_STAMP=${dcusa_stamp}
                -P "${dcusa_translate_script}"
        RESULT_VARIABLE dcusa_translate_rc)
    if(NOT dcusa_translate_rc EQUAL 0)
        message(FATAL_ERROR "translation failed (${dcusa_translate_rc})")
    endif()
    if(NOT EXISTS "${dcusa_generated_dir}/sources.txt")
        message(FATAL_ERROR "translation did not write ${dcusa_generated_dir}/sources.txt")
    endif()
    message(STATUS "Translating edition ${dcusa_edition} from ${DCUSA_PREPARED_CACHE}")
elseif(DCUSA_GENERATED_DIR)
    set(dcusa_generated_dir "${DCUSA_GENERATED_DIR}")
endif()

if(dcusa_generated_dir)
    if(NOT EXISTS "${dcusa_generated_dir}/sources.txt")
        message(FATAL_ERROR "DCUSA_GENERATED_DIR has no sources.txt: run tools/x86tr/translate.py first")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dcusa_generated_dir}/sources.txt")
    file(STRINGS "${dcusa_generated_dir}/sources.txt" dcusa_generated_names)
    list(TRANSFORM dcusa_generated_names PREPEND "${dcusa_generated_dir}/")
    if(DCUSA_PREPARED_CACHE)
        add_custom_command(
            OUTPUT "${dcusa_stamp}"
            COMMAND ${CMAKE_COMMAND}
                    -DDCUSA_CACHE=${DCUSA_PREPARED_CACHE}
                    -DDCUSA_X86TR_DIR=${dcusa_x86tr_dir}
                    -DDCUSA_GEN_DIR=${dcusa_generated_dir}
                    -DDCUSA_PYTHON=${Python3_EXECUTABLE}
                    -DDCUSA_PYTHONPATH=${DCUSA_PYTHONPATH}
                    -DDCUSA_SOURCE=${CMAKE_SOURCE_DIR}
                    -DDCUSA_STAMP=${dcusa_stamp}
                    -P "${dcusa_translate_script}"
            DEPENDS "${DCUSA_PREPARED_CACHE}" "${dcusa_translate_script}" ${dcusa_x86tr_scripts}
            COMMENT "Discover and translate Daytona.exe"
            VERBATIM USES_TERMINAL)
        add_custom_target(dcusa_translate DEPENDS "${dcusa_stamp}")
    endif()
    add_library(dcusa_generated STATIC ${dcusa_generated_names})
    target_link_libraries(dcusa_generated PUBLIC dcusa_recomp)
    if(TARGET dcusa_translate)
        add_dependencies(dcusa_generated dcusa_translate)
    endif()
    # Same floating-point rules as the runtime; no warnings (the code mirrors the
    # guest instruction by instruction) and no debug info (it would run to gigabytes).
    set(DCUSA_GENERATED_OPT "-O2" CACHE STRING "optimisation level for the translated code")
    target_compile_options(dcusa_generated PRIVATE -w ${DCUSA_GENERATED_OPT} -g0 -ffp-contract=off -fno-fast-math)
    # Profile-guided optimisation of the translated code and the runtime (PHASE3
    # "Frame-rate profile"): DCUSA_PGO=generate builds an instrumented dcrun, whose
    # run writes LLVM_PROFILE_FILE; DCUSA_PGO=<merged .profdata> builds with it.
    # A linker order file (one symbol per line) puts the functions a run used
    # next to each other, for the instruction cache and TLB (PHASE3).
    set(DCUSA_ORDER_FILE "" CACHE FILEPATH "linker order file for dcrun")
    if(DCUSA_ORDER_FILE)
        target_link_options(dcusa_generated INTERFACE "LINKER:-order_file,${DCUSA_ORDER_FILE}")
    endif()
    set(DCUSA_PGO "" CACHE STRING "generate, or a merged .profdata to optimise with")
    if(DCUSA_PGO STREQUAL "generate")
        target_compile_options(dcusa_generated PRIVATE -fprofile-instr-generate)
        target_compile_options(dcusa_recomp PRIVATE -fprofile-instr-generate)
        target_compile_definitions(dcusa_recomp PRIVATE DCUSA_PGO_GENERATE=1)
        target_link_options(dcusa_generated INTERFACE -fprofile-instr-generate)
    elseif(DCUSA_PGO)
        target_compile_options(dcusa_generated PRIVATE "-fprofile-instr-use=${DCUSA_PGO}")
        target_compile_options(dcusa_recomp PRIVATE "-fprofile-instr-use=${DCUSA_PGO}" -Wno-profile-instr-unprofiled
                               -Wno-profile-instr-out-of-date)
    endif()

    if(NOT DCUSA_IOS)
        add_executable(dcrun tools/dcrun/main.cpp src/platform/macos/gamecontroller_pad.mm
                             src/platform/macos/game_window.mm src/platform/macos/core_audio_output.mm)
        set_source_files_properties(src/platform/macos/gamecontroller_pad.mm src/platform/macos/game_window.mm
                                    src/platform/macos/core_audio_output.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(dcrun PRIVATE dcusa_generated dcusa_system "-framework GameController" "-framework AppKit"
                                            "-framework QuartzCore" "-framework AudioToolbox")
        dcusa_warnings(dcrun)
    else()
        # The iPhone / iPad app. Simulator builds are signed ad hoc after linking;
        # a device build needs a development team (DCUSA_IOS_TEAM) and the
        # extended virtual addressing entitlement (the 4 GB guest space).
        add_executable(DaytonaUSA MACOSX_BUNDLE src/platform/ios/ios_app.mm src/platform/macos/gamecontroller_pad.mm
                                                src/platform/macos/core_audio_output.mm)
        set_source_files_properties(src/platform/ios/ios_app.mm src/platform/macos/gamecontroller_pad.mm
                                    src/platform/macos/core_audio_output.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set(DCUSA_IOS_BUNDLE_ID "local.dcusa.DaytonaUSA" CACHE STRING "bundle identifier of the iOS app")
        set_target_properties(DaytonaUSA PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${DCUSA_IOS_BUNDLE_ID}"
            MACOSX_BUNDLE_BUNDLE_NAME "DaytonaUSA"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
            MACOSX_BUNDLE_BUNDLE_VERSION "1")
        target_link_libraries(DaytonaUSA PRIVATE dcusa_generated dcusa_system "-framework UIKit" "-framework GameController"
                                                 "-framework QuartzCore" "-framework AudioToolbox" "-framework AVFoundation"
                                                 "-framework Metal" "-framework Foundation")
        if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            add_custom_command(TARGET DaytonaUSA POST_BUILD
                COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:DaytonaUSA>"
                COMMENT "Signing DaytonaUSA.app ad hoc for the simulator")
        endif()
    endif()
endif()

if(NOT DCUSA_IOS)

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
dcusa_test(test_loader)
dcusa_test(test_native)
dcusa_test(test_imports)
dcusa_test(test_threads)
dcusa_test(test_metal)
add_executable(test_bc_decode tests/test_bc_decode.mm)
target_link_libraries(test_bc_decode PRIVATE dcusa_recomp)
target_compile_options(test_bc_decode PRIVATE -fobjc-arc)
add_test(NAME test_bc_decode COMMAND test_bc_decode)
set_tests_properties(test_bc_decode PROPERTIES SKIP_RETURN_CODE 77)
dcusa_test(test_shell)
dcusa_test(test_pad)
dcusa_test(test_ds_mixer)

# Prepared MSL compiles with the system Metal device. The shaders come from the
# player's executable (tools/shaderprep), so the test skips where none exist.
if(APPLE)
    add_executable(mslcheck tools/mslcheck/main.mm)
    target_compile_options(mslcheck PRIVATE -fobjc-arc)
    target_link_libraries(mslcheck PRIVATE "-framework Foundation" "-framework Metal")
    set(DCUSA_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/prepared/shaders" CACHE PATH
        "Folder of captured and translated shaders (src/, msl/)")
    add_test(NAME test_msl COMMAND mslcheck "${DCUSA_SHADER_DIR}/msl")
    set_tests_properties(test_msl PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Translator semantics against real x86: tools/x86tr/semtest.py runs every case
# natively as x86_64 under Rosetta 2 and translates the same snippet assembled
# for i386; the test checks the translation reproduces the x86 results.
find_package(Python3 COMPONENTS Interpreter)
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

# ---- Phase 5: autoplay and compare (label phase5; minutes each, so leave them
# out of routine runs with `ctest -LE phase5`). They need the player's game
# folder and prepared cache; phase5_attract also needs a Windows capture of
# the attract loop (DCUSA_REFERENCE_VIDEO). Missing inputs skip (77).
set(DCUSA_REFERENCE_VIDEO "${DCUSA_GAME_LIBRARY}/../Daytona Championship USA Reference Footage/Daytona Championship USA New Season Edition Attract Mode.mp4"
    CACHE FILEPATH "A Windows capture of the attract loop, for phase5_attract")
if(TARGET dcrun AND DCUSA_PREPARED_CACHE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME phase5_controls
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/phase5/controls_smoke.py
                     --dcrun $<TARGET_FILE:dcrun> --cache ${DCUSA_PREPARED_CACHE} --content ${DCUSA_GAME_LIBRARY}
                     --out ${CMAKE_CURRENT_BINARY_DIR}/phase5/controls)
    add_test(NAME phase5_attract
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/phase5/attract_compare.py
                     --dcrun $<TARGET_FILE:dcrun> --cache ${DCUSA_PREPARED_CACHE} --content ${DCUSA_GAME_LIBRARY}
                     --reference ${DCUSA_REFERENCE_VIDEO} --out ${CMAKE_CURRENT_BINARY_DIR}/phase5/attract)
    set_tests_properties(phase5_controls phase5_attract PROPERTIES LABELS phase5 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    set_tests_properties(phase5_controls PROPERTIES TIMEOUT 600)
    set_tests_properties(phase5_attract PROPERTIES TIMEOUT 1200)
endif()
set_tests_properties(test_game_files PROPERTIES LABELS "game-files" TIMEOUT 300)
endif()  # NOT DCUSA_IOS
