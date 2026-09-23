# SPDX-License-Identifier: GPL-3.0-or-later
# check_locale.cmake — the plugin's translation files are well-formed, and every
# string the code asks for exists in en-US.
#
# Why. A missing line break in en-US.ini (113b4f6) glued `Dock.End="End
# broadcast"` onto the end of the line before it. OBS reads each line as one
# Key="value", so the key vanished, and the encoder dock's End button said
# "Dock.End" for a day — visible only on screen, only in the one language, and
# nothing in the build or the tests could see it. en-US is the one that
# matters most: OBS loads it first and lays any other locale over it, so a key
# missing from en-GB still falls back, but a key missing from en-US shows raw.
#
# Run as:  cmake -DROOT=<source dir> -P tests/check_locale.cmake
# A CMake script rather than a C++ test so it needs nothing but CMake, on every
# platform CI builds on.

if(NOT ROOT)
    message(FATAL_ERROR "pass -DROOT=<source dir>")
endif()

set(failures 0)

# 1. Every line of every locale file is blank, a comment, or exactly one
#    Key="value" — no second key hiding after the closing quote.
file(GLOB locales "${ROOT}/data/locale/*.ini")
set(us_keys "")
foreach(f IN LISTS locales)
    file(STRINGS "${f}" lines ENCODING UTF-8)
    get_filename_component(name "${f}" NAME)
    set(n 0)
    foreach(line IN LISTS lines)
        math(EXPR n "${n} + 1")
        if(line MATCHES "^[ \t]*$" OR line MATCHES "^[#;]")
            continue()
        endif()
        if(NOT line MATCHES "^([A-Za-z][A-Za-z0-9_.]*)=\"([^\"\\\\]|\\\\.)*\"$")
            message(SEND_ERROR "${name}:${n}: not one Key=\"value\" line: ${line}")
            math(EXPR failures "${failures} + 1")
            continue()
        endif()
        if(name STREQUAL "en-US.ini")
            list(APPEND us_keys "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

# 2. Every tr_("Key") in the plugin's source is defined in en-US.
file(GLOB_RECURSE sources "${ROOT}/src/obs/*.cpp" "${ROOT}/src/obs/*.h")
set(used "")
foreach(s IN LISTS sources)
    file(READ "${s}" text)
    string(REGEX MATCHALL "tr_\\(\"[A-Za-z0-9_.]+\"\\)" calls "${text}")
    foreach(c IN LISTS calls)
        string(REGEX REPLACE "^tr_\\(\"([A-Za-z0-9_.]+)\"\\)$" "\\1" key "${c}")
        list(APPEND used "${key}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES used)
foreach(key IN LISTS used)
    list(FIND us_keys "${key}" at)
    if(at EQUAL -1)
        message(SEND_ERROR "en-US.ini has no ${key}, which the plugin asks for — "
                           "OBS would show the key itself")
        math(EXPR failures "${failures} + 1")
    endif()
endforeach()

list(LENGTH used n_used)
list(LENGTH us_keys n_us)
if(failures GREATER 0)
    message(FATAL_ERROR "${failures} locale problem(s)")
endif()
message(STATUS "locale ok: ${n_us} en-US keys, all ${n_used} used keys defined")
