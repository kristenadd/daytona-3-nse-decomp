# Discover and translate a prepared image. Used at configure time and as the
# build step. Ninja runs a new custom command once just because it is new, so
# this exits immediately when sources.txt is already newer than the cache and
# the translator scripts.
if(NOT DEFINED DCUSA_CACHE OR NOT DEFINED DCUSA_X86TR_DIR OR NOT DEFINED DCUSA_GEN_DIR
   OR NOT DEFINED DCUSA_PYTHON OR NOT DEFINED DCUSA_PYTHONPATH OR NOT DEFINED DCUSA_SOURCE)
    message(FATAL_ERROR "translate.cmake is missing an input path")
endif()

set(dcusa_sources "${DCUSA_GEN_DIR}/sources.txt")
set(dcusa_stale FALSE)
if(NOT EXISTS "${dcusa_sources}")
    set(dcusa_stale TRUE)
elseif("${DCUSA_CACHE}" IS_NEWER_THAN "${dcusa_sources}")
    set(dcusa_stale TRUE)
else()
    file(GLOB dcusa_scripts "${DCUSA_SOURCE}/tools/x86tr/*.py")
    foreach(dcusa_script IN LISTS dcusa_scripts)
        if("${dcusa_script}" IS_NEWER_THAN "${dcusa_sources}")
            set(dcusa_stale TRUE)
        endif()
    endforeach()
endif()

if(dcusa_stale)
    message(STATUS "Discovering and translating ${DCUSA_CACHE}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${DCUSA_PYTHONPATH}" ${DCUSA_PYTHON}
                "${DCUSA_SOURCE}/tools/x86tr/discover.py" "${DCUSA_CACHE}" "${DCUSA_X86TR_DIR}"
        RESULT_VARIABLE dcusa_rc)
    if(NOT dcusa_rc EQUAL 0)
        message(FATAL_ERROR "discover.py failed (${dcusa_rc})")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "PYTHONPATH=${DCUSA_PYTHONPATH}" ${DCUSA_PYTHON}
                "${DCUSA_SOURCE}/tools/x86tr/translate.py" "${DCUSA_CACHE}"
                "${DCUSA_X86TR_DIR}/functions.json" "${DCUSA_GEN_DIR}"
        RESULT_VARIABLE dcusa_rc)
    if(NOT dcusa_rc EQUAL 0)
        message(FATAL_ERROR "translate.py failed (${dcusa_rc})")
    endif()
endif()

if(DCUSA_STAMP)
    file(TOUCH "${DCUSA_STAMP}")
endif()
