# The launcher logo and app icons: the generic ones in the repository, or your
# own named in branding.ini beside CMakeLists.txt (not committed; see
# tools/branding/README.md). Lines are `key = path`, the path absolute or
# relative to branding.ini; a key left out keeps the generic picture.
#
# dcusa_branding(<key> <default path> <out var>) sets <out var> to the file to use.
set(DCUSA_BRANDING_FILE "${CMAKE_CURRENT_SOURCE_DIR}/branding.ini" CACHE FILEPATH
    "logo and icon remap file (tools/branding/README.md)")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DCUSA_BRANDING_FILE}")
set(dcusa_branding_lines "")
if(EXISTS "${DCUSA_BRANDING_FILE}")
    file(STRINGS "${DCUSA_BRANDING_FILE}" dcusa_branding_lines)
endif()

function(dcusa_branding key default out)
    set(path "${default}")
    foreach(line IN LISTS dcusa_branding_lines)
        if(line MATCHES "^[ \t]*${key}[ \t]*=[ \t]*(.*[^ \t\r])[ \t\r]*$")
            set(value "${CMAKE_MATCH_1}")
            get_filename_component(dir "${DCUSA_BRANDING_FILE}" DIRECTORY)
            get_filename_component(value "${value}" ABSOLUTE BASE_DIR "${dir}")
            if(EXISTS "${value}")
                set(path "${value}")
                message(STATUS "Branding: ${key} from ${value}")
            else()
                message(WARNING "branding.ini: ${key} = ${value} does not exist; using the generic one")
            endif()
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    set(${out} "${path}" PARENT_SCOPE)
endfunction()
