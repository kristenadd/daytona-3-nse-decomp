# Writes build_info.hpp: the commit (with + when the tree has changes) and the
# build time, shown on the boot screen and with the frame rate so a run shows
# which build it is. Rewritten only when the text changes.
execute_process(COMMAND git -C "${SRC}" rev-parse --short HEAD OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no OUTPUT_VARIABLE dirty ERROR_QUIET)
if(NOT commit)
    set(commit "dev")
endif()
if(dirty)
    set(commit "${commit}+")
endif()
string(TIMESTAMP stamp "%m-%d %H:%M")
set(text "#pragma once\n#define DCUSA_BUILD_ID \"${commit} ${stamp}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT "${old}" STREQUAL "${text}")
    file(WRITE "${OUT}" "${text}")
endif()
