# Host-only unit tests for lib9p. Never installed.

foreach(name test_encode test_decode test_idpool test_fuzz)
    add_executable(${name} ${name}.c)
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    target_compile_options(${name} PRIVATE ${FS9P_STRICT_FLAGS})
    target_link_libraries(${name} PRIVATE lib9p)
    add_test(NAME ${name} COMMAND ${name})
endforeach()
