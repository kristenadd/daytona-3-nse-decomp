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
# visionOS (the visionos-sim / visionos-device presets) builds the same app,
# shown in a floating 16:9 window.
set(DCUSA_IOS OFF)
set(DCUSA_VISIONOS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(DCUSA_IOS ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(DCUSA_IOS ON)
    set(DCUSA_VISIONOS ON)
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

# The platform layer: apple (Metal, AppKit / UIKit, Core Audio,
# GameController) or sdl (Vulkan, SDL3: Linux and the Steam Deck; on a Mac,
# through MoltenVK, to test that code). Empty: apple on Apple, sdl elsewhere.
set(DCUSA_PLATFORM "" CACHE STRING "platform layer: apple or sdl (empty: apple on Apple systems, sdl elsewhere)")
if(NOT DCUSA_PLATFORM)
    if(APPLE)
        set(DCUSA_PLATFORM apple)
    else()
        set(DCUSA_PLATFORM sdl)
    endif()
endif()
if(DCUSA_PLATFORM STREQUAL "sdl")
    find_package(SDL3 REQUIRED CONFIG)
    find_program(DCUSA_GLSLANG NAMES glslangValidator glslang REQUIRED)
elseif(NOT DCUSA_PLATFORM STREQUAL "apple")
    message(FATAL_ERROR "DCUSA_PLATFORM must be apple or sdl")
endif()
message(STATUS "Platform layer: ${DCUSA_PLATFORM}")

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
    src/system/files.cpp
    src/system/shell_settings.cpp)
target_include_directories(dcusa_system PUBLIC src)
target_link_libraries(dcusa_system PUBLIC ZLIB::ZLIB)
dcusa_warnings(dcusa_system)

# ---- launcher: the title, OPTIONS and CONTROLS screens shown before the game ----
# Platform-free (src/platform/launcher): the platforms draw its triangles with
# SDL or Metal and play its sound. Fonts and logo are compiled in.
set(dcusa_launcher_assets "${CMAKE_CURRENT_BINARY_DIR}/launcher_assets.cpp")
add_custom_command(OUTPUT "${dcusa_launcher_assets}"
    COMMAND ${CMAKE_COMMAND}
            "-DIN_FILES=title_font=${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/RussoOne-Regular.ttf\;body_font=${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/TitilliumWeb-Bold.ttf\;logo=${CMAKE_CURRENT_SOURCE_DIR}/src/platform/launcher/logo.png"
            "-DOUT=${dcusa_launcher_assets}" -P "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/launcher/embed.cmake"
    DEPENDS third_party/fonts/RussoOne-Regular.ttf third_party/fonts/TitilliumWeb-Bold.ttf
            src/platform/launcher/logo.png src/platform/launcher/embed.cmake
    COMMENT "Embedding the launcher's fonts and logo")
add_library(dcusa_launcher STATIC
    src/platform/launcher/launcher.cpp
    src/platform/launcher/sound_bank.cpp
    "${dcusa_launcher_assets}")
target_include_directories(dcusa_launcher PUBLIC src)
target_link_libraries(dcusa_launcher PUBLIC dcusa_system dcusa_runtime)
target_compile_options(dcusa_launcher PRIVATE -Wall -Wextra)

# ---- runtime: fail-closed reporting and the platform contract ----
add_library(dcusa_runtime STATIC
    src/runtime/fail_closed.cpp
    src/runtime/pad_mapping.cpp
    src/runtime/pad_script.cpp)
target_include_directories(dcusa_runtime PUBLIC src)
dcusa_warnings(dcusa_runtime)

# The build stamp (cmake/build_info.cmake), refreshed at every build.
add_custom_target(dcusa_build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.hpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.hpp)

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
    src/gpu/bc_decode.cpp
    src/gpu/gpu_queue.cpp
    # meshoptimizer (MIT, third_party/meshoptimizer): the simplifier for DCUSA_LOD.
    third_party/meshoptimizer/src/simplifier.cpp
    third_party/meshoptimizer/src/allocator.cpp)
target_include_directories(dcusa_recomp PUBLIC src)
target_include_directories(dcusa_recomp PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_dependencies(dcusa_recomp dcusa_build_info)
target_link_libraries(dcusa_recomp PUBLIC dcusa_runtime)
if(DCUSA_PLATFORM STREQUAL "apple")
    target_sources(dcusa_recomp PRIVATE src/gpu/metal.mm src/gpu/boot_screen.mm)
    target_link_libraries(dcusa_recomp PUBLIC "-framework Foundation" "-framework Metal" "-framework QuartzCore"
                          "-framework CoreGraphics" "-framework CoreText" "-framework ImageIO")
    set_source_files_properties(src/gpu/gpu_queue.cpp PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(src/gpu/metal.mm src/gpu/gpu_queue.cpp PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
    # The Vulkan renderer's own shaders (the frame shown in the window),
    # compiled to SPIR-V headers.
    set(dcusa_shader_out "${CMAKE_CURRENT_BINARY_DIR}/generated")
    foreach(dcusa_stage vert frag)
        set(dcusa_var kShow${dcusa_stage})
        string(REPLACE "kShowvert" "kShowVert" dcusa_var "${dcusa_var}")
        string(REPLACE "kShowfrag" "kShowFrag" dcusa_var "${dcusa_var}")
        add_custom_command(
            OUTPUT "${dcusa_shader_out}/show_${dcusa_stage}.h"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${dcusa_shader_out}"
            COMMAND ${DCUSA_GLSLANG} -V --vn ${dcusa_var} -o "${dcusa_shader_out}/show_${dcusa_stage}.h"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/show.${dcusa_stage}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/show.${dcusa_stage}"
            COMMENT "SPIR-V for show.${dcusa_stage}")
    endforeach()
    target_sources(dcusa_recomp PRIVATE src/gpu/vulkan.cpp src/gpu/boot_screen_stb.cpp
                   "${dcusa_shader_out}/show_vert.h" "${dcusa_shader_out}/show_frag.h")
    target_link_libraries(dcusa_recomp PUBLIC SDL3::SDL3 ${CMAKE_DL_LIBS})
    target_compile_definitions(dcusa_recomp PUBLIC DCUSA_PLATFORM_SDL=1)
    if(APPLE)
        target_include_directories(dcusa_recomp PRIVATE /opt/homebrew/include)  # Vulkan headers (brew vulkan-headers)
    endif()
endif()
# d3d11.cpp runs tools/shaderprep on a shader it has no translation for (DCUSA_TRANSLATE=1).
set_source_files_properties(src/recomp/d3d11.cpp PROPERTIES COMPILE_DEFINITIONS "DCUSA_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
dcusa_warnings(dcusa_recomp)

# ---- translated game code ----
# tools/x86tr generates it from the player's prepared EXE images. Set
# DCUSA_PREPARED_CACHE to one cache or a list of them (one per edition,
# separated by ;) and the build runs discovery and translation itself. Each
# edition's code sits in its own namespace with its own entry table, so one
# binary can hold both and pick one at start (recomp/generated.hpp).
# DCUSA_GENERATED_DIR remains for folders that were translated by hand.
set(DCUSA_PREPARED_CACHE "" CACHE STRING
    "dcprepare cache(s), ;-separated. When set, discovery and translation run as part of the build")
set(DCUSA_GENERATED_DIR "" CACHE STRING
    "translate.py output folder(s) (each holds sources.txt); empty to build without game code")
set(DCUSA_PYTHONPATH "${CMAKE_CURRENT_SOURCE_DIR}/build/pylib" CACHE PATH
    "PYTHONPATH for tools/x86tr (where capstone is installed)")

set(dcusa_editions "")          # editions built in, e.g. 2019;2017
set(dcusa_generated_names "")   # every edition's translated sources
set(dcusa_translate_targets "")
set(dcusa_cache_2019 "")        # the 2019 cache, for the Phase 5 tests
set(dcusa_translate_script "${CMAKE_SOURCE_DIR}/cmake/translate.cmake")
file(GLOB dcusa_x86tr_scripts CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/x86tr/*.py")
foreach(dcusa_cache IN LISTS DCUSA_PREPARED_CACHE)
    if(NOT EXISTS "${dcusa_cache}")
        message(FATAL_ERROR "DCUSA_PREPARED_CACHE does not exist: ${dcusa_cache}")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Cache header: 8-byte magic, then five little-endian u32s. Edition is the second.
    file(READ "${dcusa_cache}" dcusa_hdr LIMIT 16 HEX)
    string(TOLOWER "${dcusa_hdr}" dcusa_hdr)
    string(SUBSTRING "${dcusa_hdr}" 0 16 dcusa_magic)
    if(NOT dcusa_magic STREQUAL "4443555341494d47")
        message(FATAL_ERROR "DCUSA_PREPARED_CACHE is not a prepared image: ${dcusa_cache}")
    endif()
    string(SUBSTRING "${dcusa_hdr}" 24 2 dcusa_e0)
    string(SUBSTRING "${dcusa_hdr}" 26 2 dcusa_e1)
    string(SUBSTRING "${dcusa_hdr}" 28 2 dcusa_e2)
    string(SUBSTRING "${dcusa_hdr}" 30 2 dcusa_e3)
    math(EXPR dcusa_edition "0x${dcusa_e0} + 0x${dcusa_e1} * 256 + 0x${dcusa_e2} * 65536 + 0x${dcusa_e3} * 16777216")
    if(NOT dcusa_edition EQUAL 2017 AND NOT dcusa_edition EQUAL 2019)
        message(FATAL_ERROR "prepared image edition ${dcusa_edition} is not 2017 or 2019")
    endif()
    if(dcusa_edition IN_LIST dcusa_editions)
        message(FATAL_ERROR "DCUSA_PREPARED_CACHE names edition ${dcusa_edition} twice")
    endif()
    if(dcusa_edition EQUAL 2019)
        set(dcusa_cache_2019 "${dcusa_cache}")
    endif()
    set(dcusa_x86tr_dir "${CMAKE_SOURCE_DIR}/build/x86tr/${dcusa_edition}")
    set(dcusa_gen "${dcusa_x86tr_dir}/gen")
    # The stamp lives in the build tree. The generated shards keep their own mtimes, so
    # they are not the custom command's outputs: an unchanged shard would look stale.
    set(dcusa_stamp "${CMAKE_CURRENT_BINARY_DIR}/x86tr-translated-${dcusa_edition}.stamp")
    set(dcusa_translate_args
        -DDCUSA_CACHE=${dcusa_cache} -DDCUSA_X86TR_DIR=${dcusa_x86tr_dir} -DDCUSA_GEN_DIR=${dcusa_gen}
        -DDCUSA_PYTHON=${Python3_EXECUTABLE} -DDCUSA_PYTHONPATH=${DCUSA_PYTHONPATH}
        -DDCUSA_SOURCE=${CMAKE_SOURCE_DIR} -DDCUSA_STAMP=${dcusa_stamp})
    execute_process(COMMAND ${CMAKE_COMMAND} ${dcusa_translate_args} -P "${dcusa_translate_script}"
                    RESULT_VARIABLE dcusa_translate_rc)
    if(NOT dcusa_translate_rc EQUAL 0)
        message(FATAL_ERROR "translation of ${dcusa_edition} failed (${dcusa_translate_rc})")
    endif()
    if(NOT EXISTS "${dcusa_gen}/sources.txt")
        message(FATAL_ERROR "translation did not write ${dcusa_gen}/sources.txt")
    endif()
    message(STATUS "Translating edition ${dcusa_edition} from ${dcusa_cache}")
    add_custom_command(
        OUTPUT "${dcusa_stamp}"
        COMMAND ${CMAKE_COMMAND} ${dcusa_translate_args} -P "${dcusa_translate_script}"
        DEPENDS "${dcusa_cache}" "${dcusa_translate_script}" ${dcusa_x86tr_scripts}
        COMMENT "Discover and translate Daytona.exe (${dcusa_edition})"
        VERBATIM USES_TERMINAL)
    add_custom_target(dcusa_translate_${dcusa_edition} DEPENDS "${dcusa_stamp}")
    list(APPEND dcusa_translate_targets dcusa_translate_${dcusa_edition})
    list(APPEND dcusa_editions ${dcusa_edition})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dcusa_gen}/sources.txt")
    file(STRINGS "${dcusa_gen}/sources.txt" dcusa_names)
    list(TRANSFORM dcusa_names PREPEND "${dcusa_gen}/")
    list(APPEND dcusa_generated_names ${dcusa_names})
    set(dcusa_generated_dir_${dcusa_edition} "${dcusa_gen}")
endforeach()
# Hand-translated folders: the edition comes from their coverage.json.
foreach(dcusa_gen IN LISTS DCUSA_GENERATED_DIR)
    if(NOT EXISTS "${dcusa_gen}/sources.txt" OR NOT EXISTS "${dcusa_gen}/coverage.json")
        message(FATAL_ERROR "DCUSA_GENERATED_DIR ${dcusa_gen} has no sources.txt / coverage.json: run tools/x86tr/translate.py first")
    endif()
    file(READ "${dcusa_gen}/coverage.json" dcusa_cov LIMIT 200)
    string(REGEX MATCH "\"edition\": *([0-9]+)" dcusa_m "${dcusa_cov}")
    set(dcusa_edition "${CMAKE_MATCH_1}")
    if(dcusa_edition IN_LIST dcusa_editions)
        message(WARNING "DCUSA_GENERATED_DIR ${dcusa_gen}: edition ${dcusa_edition} already comes from DCUSA_PREPARED_CACHE; ignored")
        continue()
    endif()
    list(APPEND dcusa_editions ${dcusa_edition})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${dcusa_gen}/sources.txt")
    file(STRINGS "${dcusa_gen}/sources.txt" dcusa_names)
    list(TRANSFORM dcusa_names PREPEND "${dcusa_gen}/")
    list(APPEND dcusa_generated_names ${dcusa_names})
    set(dcusa_generated_dir_${dcusa_edition} "${dcusa_gen}")
endforeach()

if(dcusa_editions)
    add_library(dcusa_generated STATIC ${dcusa_generated_names} src/recomp/generated_select.cpp)
    target_link_libraries(dcusa_generated PUBLIC dcusa_recomp)
    foreach(dcusa_edition IN LISTS dcusa_editions)
        target_compile_definitions(dcusa_generated PRIVATE DCUSA_HAS_EDITION_${dcusa_edition}=1)
    endforeach()
    message(STATUS "Game editions built in: ${dcusa_editions}")
    if(dcusa_translate_targets)
        add_dependencies(dcusa_generated ${dcusa_translate_targets})
    endif()
    # Same floating-point rules as the runtime; no warnings (the code mirrors the
    # guest instruction by instruction) and no debug info (it would run to gigabytes).
    set(DCUSA_GENERATED_OPT "-O2" CACHE STRING "optimisation level for the translated code")
    target_compile_options(dcusa_generated PRIVATE -w ${DCUSA_GENERATED_OPT} -g0 -ffp-contract=off -fno-fast-math)
    # DCUSA_GENERATED_LINES: translated files (names such as tr_0043A870.cpp)
    # built with line tables, so a profile maps samples to source lines and,
    # through their comments, to guest instructions.
    set(DCUSA_GENERATED_LINES "" CACHE STRING "translated files to build with line tables, or all (for profiling)")
    if(DCUSA_GENERATED_LINES STREQUAL "all")
        target_compile_options(dcusa_generated PRIVATE -gline-tables-only)
        # The Xcode generator turns -g flags into its own settings.
        set_target_properties(dcusa_generated PROPERTIES XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS YES
                              XCODE_ATTRIBUTE_CLANG_DEBUG_INFORMATION_LEVEL line-tables-only)
    else()
        foreach(f IN LISTS DCUSA_GENERATED_LINES)
            foreach(dcusa_edition IN LISTS dcusa_editions)
                if(EXISTS "${dcusa_generated_dir_${dcusa_edition}}/${f}")
                    set_source_files_properties("${dcusa_generated_dir_${dcusa_edition}}/${f}" TARGET_DIRECTORY
                                                dcusa_generated PROPERTIES COMPILE_OPTIONS "-gline-tables-only")
                endif()
            endforeach()
        endforeach()
    endif()
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

    if(NOT DCUSA_IOS AND DCUSA_PLATFORM STREQUAL "sdl")
        add_executable(dcrun tools/dcrun/main.cpp src/platform/sdl/sdl_platform.cpp src/platform/edition_choice.cpp)
        target_link_libraries(dcrun PRIVATE dcusa_generated dcusa_system dcusa_launcher SDL3::SDL3)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # Libraries packaged beside the binary (tools/linux/package.sh) are found there.
            set_target_properties(dcrun PROPERTIES BUILD_RPATH "$ORIGIN/lib" INSTALL_RPATH "$ORIGIN/lib")
            target_link_libraries(dcrun PRIVATE pthread)
        endif()
        dcusa_warnings(dcrun)
    elseif(NOT DCUSA_IOS)
        add_executable(dcrun tools/dcrun/main.cpp src/platform/macos/gamecontroller_pad.mm
                             src/platform/macos/game_window.mm src/platform/macos/core_audio_output.mm
                             src/platform/launcher/launcher_metal.mm src/platform/edition_choice.cpp)
        set_source_files_properties(src/platform/macos/gamecontroller_pad.mm src/platform/macos/game_window.mm
                                    src/platform/macos/core_audio_output.mm src/platform/launcher/launcher_metal.mm
                                    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(dcrun PRIVATE dcusa_generated dcusa_system dcusa_launcher "-framework GameController"
                                            "-framework AppKit" "-framework QuartzCore" "-framework AudioToolbox"
                                            "-framework Metal")
        dcusa_warnings(dcrun)
    else()
        # The iPhone / iPad app. Simulator builds are signed ad hoc after linking;
        # a device build needs a development team (DCUSA_IOS_TEAM) and the
        # extended virtual addressing entitlement (the 4 GB guest space).
        add_executable(DaytonaUSA MACOSX_BUNDLE src/platform/ios/ios_app.mm src/platform/macos/gamecontroller_pad.mm
                                                src/platform/macos/core_audio_output.mm
                                                src/platform/launcher/launcher_metal.mm src/platform/edition_choice.cpp)
        set_source_files_properties(src/platform/ios/ios_app.mm src/platform/macos/gamecontroller_pad.mm
                                    src/platform/macos/core_audio_output.mm src/platform/launcher/launcher_metal.mm
                                    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        set(DCUSA_IOS_BUNDLE_ID "local.dcusa.DaytonaUSA" CACHE STRING "bundle identifier of the iOS app")
        if(DCUSA_VISIONOS)
            set(dcusa_info_plist "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Info.vision.plist.in")
        else()
            set(dcusa_info_plist "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Info.plist.in")
        endif()
        set_target_properties(DaytonaUSA PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${dcusa_info_plist}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${DCUSA_IOS_BUNDLE_ID}"
            MACOSX_BUNDLE_BUNDLE_NAME "DaytonaUSA"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
            MACOSX_BUNDLE_BUNDLE_VERSION "1")
        # The app icon: the asset catalog compiled into the bundle (Assets.car and
        # the 60/76 pt PNGs that Info.plist names), before the bundle is signed.
        if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            set(DCUSA_IOS_PLATFORM iphonesimulator)
        else()
            set(DCUSA_IOS_PLATFORM iphoneos)
        endif()
        # (visionOS: no icon yet; its icons are layered image stacks.)
        if(NOT DCUSA_VISIONOS)
        add_custom_command(TARGET DaytonaUSA POST_BUILD
            COMMAND xcrun actool --compile "$<TARGET_BUNDLE_DIR:DaytonaUSA>"
                    --platform ${DCUSA_IOS_PLATFORM}
                    --minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET}
                    --target-device iphone --target-device ipad --app-icon AppIcon
                    --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/assets-partial.plist"
                    --output-format human-readable-text --errors --warnings
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Assets.xcassets"
            COMMENT "Compiling the app icon")
        endif()
        # The launch screen and the boot screen's title: the attract title made
        # from the player's game on the Mac (tools/bootscreen/capture_title.py),
        # never committed. Without it the launch screen is black and the boot
        # screen is off. The Settings app's switch for the boot screen.
        set(DCUSA_BOOT_TITLE "${CMAKE_CURRENT_SOURCE_DIR}/build/mac-host/prepared/boot/title.png" CACHE FILEPATH
            "attract title for the iOS launch and boot screens (capture_title.py)")
        if(EXISTS "${DCUSA_BOOT_TITLE}")
            add_custom_command(TARGET DaytonaUSA POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy "${DCUSA_BOOT_TITLE}" "$<TARGET_BUNDLE_DIR:DaytonaUSA>/boot_title.png"
                COMMENT "Copying the boot title")
        else()
            message(STATUS "No boot title at ${DCUSA_BOOT_TITLE}: launch screen black, boot screen off")
        endif()
        # The label's font, if one is beside the title (font.ttf; not committed).
        get_filename_component(dcusa_boot_dir "${DCUSA_BOOT_TITLE}" DIRECTORY)
        # The 2017 edition's title (title2017.png beside it), for the edition
        # picker and that edition's boot screen.
        if(EXISTS "${dcusa_boot_dir}/title2017.png")
            add_custom_command(TARGET DaytonaUSA POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy "${dcusa_boot_dir}/title2017.png"
                        "$<TARGET_BUNDLE_DIR:DaytonaUSA>/boot_title2017.png"
                COMMENT "Copying the 2017 boot title")
        endif()
        if(EXISTS "${dcusa_boot_dir}/font.ttf")
            add_custom_command(TARGET DaytonaUSA POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy "${dcusa_boot_dir}/font.ttf" "$<TARGET_BUNDLE_DIR:DaytonaUSA>/boot_font.ttf"
                COMMENT "Copying the boot screen font")
        endif()
        if(DCUSA_VISIONOS)
            add_custom_command(TARGET DaytonaUSA POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Settings.bundle"
                        "$<TARGET_BUNDLE_DIR:DaytonaUSA>/Settings.bundle"
                COMMENT "Copying Settings.bundle")
        else()
        add_custom_command(TARGET DaytonaUSA POST_BUILD
            COMMAND xcrun ibtool --compile "$<TARGET_BUNDLE_DIR:DaytonaUSA>/LaunchScreen.storyboardc"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/LaunchScreen.storyboard"
                    --target-device iphone --target-device ipad --minimum-deployment-target ${CMAKE_OSX_DEPLOYMENT_TARGET}
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/Settings.bundle"
                    "$<TARGET_BUNDLE_DIR:DaytonaUSA>/Settings.bundle"
            COMMENT "Compiling the launch screen, copying Settings.bundle")
        endif()
        target_link_libraries(DaytonaUSA PRIVATE dcusa_generated dcusa_system dcusa_launcher "-framework UIKit" "-framework GameController"
                                                 "-framework QuartzCore" "-framework AudioToolbox" "-framework AVFoundation"
                                                 "-framework Metal" "-framework Foundation")
        if(CMAKE_GENERATOR STREQUAL "Xcode" AND NOT CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            # A device: Xcode's automatic signing registers the app ID with these
            # entitlements and fetches a profile for the team's devices
            # (cmake --build --preset ios-device passes -allowProvisioningUpdates).
            set(DCUSA_IOS_TEAM "" CACHE STRING "Apple development team ID for device builds")
            set(dcusa_device_family "1,2")  # iPhone, iPad
            if(DCUSA_VISIONOS)
                set(dcusa_device_family "7")  # Apple Vision Pro
            endif()
            if(NOT DCUSA_IOS_TEAM)
                message(FATAL_ERROR "A device build needs -DDCUSA_IOS_TEAM=<team id>")
            endif()
            set_target_properties(DaytonaUSA PROPERTIES
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${DCUSA_IOS_BUNDLE_ID}"
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${DCUSA_IOS_TEAM}"
                XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ios/DaytonaUSA.entitlements"
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "${dcusa_device_family}")
        elseif(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
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
if(DCUSA_PLATFORM STREQUAL "apple")
dcusa_test(test_metal)
add_executable(test_bc_decode tests/test_bc_decode.mm)
target_link_libraries(test_bc_decode PRIVATE dcusa_recomp)
target_compile_options(test_bc_decode PRIVATE -fobjc-arc)
add_test(NAME test_bc_decode COMMAND test_bc_decode)
set_tests_properties(test_bc_decode PROPERTIES SKIP_RETURN_CODE 77)
endif()
dcusa_test(test_shell)
dcusa_test(test_pad)
dcusa_test(test_ds_mixer)

# Prepared MSL compiles with the system Metal device. The shaders come from the
# player's executable (tools/shaderprep), so the test skips where none exist.
if(APPLE AND DCUSA_PLATFORM STREQUAL "apple")
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
if(TARGET dcrun AND dcusa_cache_2019)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME phase5_controls
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/phase5/controls_smoke.py
                     --dcrun $<TARGET_FILE:dcrun> --cache ${dcusa_cache_2019} --content ${DCUSA_GAME_LIBRARY}
                     --out ${CMAKE_CURRENT_BINARY_DIR}/phase5/controls)
    add_test(NAME phase5_attract
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/phase5/attract_compare.py
                     --dcrun $<TARGET_FILE:dcrun> --cache ${dcusa_cache_2019} --content ${DCUSA_GAME_LIBRARY}
                     --reference ${DCUSA_REFERENCE_VIDEO} --out ${CMAKE_CURRENT_BINARY_DIR}/phase5/attract)
    set_tests_properties(phase5_controls phase5_attract PROPERTIES LABELS phase5 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    set_tests_properties(phase5_controls PROPERTIES TIMEOUT 600)
    set_tests_properties(phase5_attract PROPERTIES TIMEOUT 1200)
endif()
set_tests_properties(test_game_files PROPERTIES LABELS "game-files" TIMEOUT 300)
endif()  # NOT DCUSA_IOS
