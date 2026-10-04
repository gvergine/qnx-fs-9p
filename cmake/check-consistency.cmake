# Consistency checks run by ctest (host builds):
#  - CHANGELOG.md has a section for the version in CMakeLists.txt;
#  - every -o option that src/main.c accepts appears in the use message
#    (src/fs-9p.use.in) and in the fs-9p reference page.
# usage: cmake -DSRC=<source dir> -DVERSION=<x.y.z> -P check-consistency.cmake

file(READ "${SRC}/CHANGELOG.md" changelog)
string(REPLACE "." "\\." version_re "${VERSION}")
if(NOT changelog MATCHES "\n## \\[${version_re}\\]")
    message(FATAL_ERROR "CHANGELOG.md has no '## [${VERSION}]' section for the version in CMakeLists.txt")
endif()

file(READ "${SRC}/src/main.c" main)
string(REGEX MATCHALL "strcmp\\(o, \"[a-z]+\"\\)" calls "${main}")
if(NOT calls)
    message(FATAL_ERROR "found no options in src/main.c: did the parser change?")
endif()
file(READ "${SRC}/src/fs-9p.use.in" use)
set(reference "${SRC}/docs/user/reference.md")
if(EXISTS "${reference}")
    file(READ "${reference}" ref)
endif()
set(missing "")
foreach(call IN LISTS calls)
    string(REGEX REPLACE "strcmp\\(o, \"([a-z]+)\"\\)" "\\1" opt "${call}")
    if(NOT use MATCHES "\n ${opt}[ =]")
        list(APPEND missing "${opt} (use message)")
    endif()
    if(DEFINED ref AND NOT ref MATCHES "`${opt}[`=]")
        list(APPEND missing "${opt} (docs/user/reference.md)")
    endif()
endforeach()
if(missing)
    message(FATAL_ERROR "options accepted by src/main.c but not documented: ${missing}")
endif()
message(STATUS "version ${VERSION}: changelog and option docs consistent")
