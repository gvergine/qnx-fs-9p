cmake_minimum_required(VERSION 3.20)
# The version lives here and only here: the use message, the docs and the
# release tag take it from this line (docs/dev/releasing.md).
project(fs-9p VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
    set(FS9P_QNX ON)
    # aarch64le only (docs/dev/architecture.md); the virtio-mmio transport is specific to QEMU virt.
    if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        message(FATAL_ERROR "fs-9p supports only aarch64le; use cmake/qnx8-aarch64le.cmake")
    endif()
else()
    set(FS9P_QNX OFF)
    message(NOTICE "fs-9p: host build: lib9p and its unit tests only, nothing to install. To build\n"
                   "  fs-9p itself: -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake (docs/user/building.md)")
endif()

if(FS9P_QNX)
    set(_fs9p_tests_default OFF)
else()
    set(_fs9p_tests_default ON)
endif()

option(FS9P_BUILD_TESTS "Build host unit tests for lib9p" ${_fs9p_tests_default})
option(FS9P_INSTALL_LIB "Install lib9p static library and headers" OFF)
option(FS9P_SANITIZE "Build with ASan/UBSan (host builds only)" OFF)

if(FS9P_BUILD_TESTS AND FS9P_QNX)
    message(FATAL_ERROR "FS9P_BUILD_TESTS is host-only; pass -DFS9P_BUILD_TESTS=OFF")
endif()

set(FS9P_STRICT_FLAGS -Wall -Wextra -Wpedantic -Werror)

if(FS9P_SANITIZE)
    if(FS9P_QNX)
        message(FATAL_ERROR "FS9P_SANITIZE is only supported on host builds")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- lib9p: portable codec, both modes -------------------------------------
add_library(lib9p STATIC
    lib9p/src/p9.c
    lib9p/src/encode.c
    lib9p/src/decode.c
    lib9p/src/idpool.c
)
set_target_properties(lib9p PROPERTIES
    OUTPUT_NAME 9p
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
target_include_directories(lib9p PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib9p/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_options(lib9p PRIVATE ${FS9P_STRICT_FLAGS})

if(FS9P_INSTALL_LIB)
    install(TARGETS lib9p ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY lib9p/include/lib9p DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

# --- fs-9p resource manager: QNX only ---------------------------------------
if(FS9P_QNX)
    add_executable(fs-9p
        src/main.c
        src/cache.c
        src/client.c
        src/errno_map.c
        src/resmgr_core.c
        src/resmgr_connect.c
        src/resmgr_io.c
        src/transport_virtio.c
    )
    set_target_properties(fs-9p PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    # QNX extensions (mmap_device_memory, InterruptAttachThread, ...) are
    # hidden under strict -std=c11.
    target_compile_definitions(fs-9p PRIVATE _QNX_SOURCE)
    target_compile_options(fs-9p PRIVATE ${FS9P_STRICT_FLAGS})
    target_link_libraries(fs-9p PRIVATE lib9p)

    # The use message, with the version filled in, embedded so `use fs-9p`
    # works. LINK_DEPENDS relinks (and so re-embeds) when the text changes.
    set(FS9P_USE_FILE ${CMAKE_CURRENT_BINARY_DIR}/fs-9p.use)
    configure_file(src/fs-9p.use.in ${FS9P_USE_FILE} @ONLY)
    set_property(TARGET fs-9p APPEND PROPERTY LINK_DEPENDS ${FS9P_USE_FILE})
    add_custom_command(TARGET fs-9p POST_BUILD
        COMMAND ${FS9P_USEMSG} $<TARGET_FILE:fs-9p> ${FS9P_USE_FILE}
        COMMENT "Embedding usage message in fs-9p"
        VERBATIM
    )

    install(TARGETS fs-9p RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif()

# --- HTML documentation: optional, never part of the default build ---------
# Sphinx + MyST + furo from docs/requirements.txt, normally in a venv at
# .venv-docs (docs/dev/docs.md). -W: a warning (a broken reference) fails.
# LC_ALL=C: Sphinx calls setlocale(LC_ALL, ""), which fails when the shell
# names a locale that isn't installed; the output doesn't depend on it, and
# every system has C (Python reads and writes UTF-8 under it).
find_program(FS9P_SPHINX_BUILD sphinx-build HINTS ${CMAKE_CURRENT_SOURCE_DIR}/.venv-docs/bin)
if(FS9P_SPHINX_BUILD)
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E env LC_ALL=C
                ${FS9P_SPHINX_BUILD} -W --keep-going -q -b html
                -d ${CMAKE_CURRENT_BINARY_DIR}/docs/doctrees
                ${CMAKE_CURRENT_SOURCE_DIR}/docs ${CMAKE_CURRENT_BINARY_DIR}/docs/html
        COMMENT "Building the HTML documentation in ${CMAKE_CURRENT_BINARY_DIR}/docs/html"
        VERBATIM
    )
endif()

# --- docs and packaging: installed with the binary, so QNX builds only -------
# A host build has no fs-9p to go with them.
if(FS9P_QNX)
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs/html
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/fs-9p OPTIONAL)
    install(FILES README.md LICENSE CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/fs-9p)
    install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/fs-9p
        FILES_MATCHING PATTERN "*.md")
    install(FILES packaging/fs-9p.build DESTINATION ${CMAKE_INSTALL_DATADIR}/fs-9p)
else()
    install(CODE "message(NOTICE \"fs-9p: host build, nothing to install. fs-9p is built with \"
                                 \"-DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake\")")
endif()

# --- host unit tests ---------------------------------------------------------
if(FS9P_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    # Version in the changelog, options in the use message and reference.
    add_test(NAME consistency
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DVERSION=${PROJECT_VERSION}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-consistency.cmake)
endif()
