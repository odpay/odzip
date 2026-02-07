cmake_minimum_required(VERSION 3.16)
project(odzip C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(odz
    main.c
    odz_util.c
    bitstream.c
    huffman.c
    lz_hashchain.c
    compress.c
    decompress.c
)

option(ODZ_PORTABLE "Build portable binary (no -march=native)" OFF)

if (MSVC)
    target_compile_options(odz PRIVATE /O2 /W4)
else()
    target_compile_options(odz PRIVATE -O2 -Wall -Wextra -pedantic -flto)
    if (NOT ODZ_PORTABLE)
        target_compile_options(odz PRIVATE -march=native)
    endif()
    target_link_options(odz PRIVATE -flto)
endif()

# roundtrip compress -> decompress license test
set(LICENSE_FILE ${CMAKE_SOURCE_DIR}/LICENSE)
if (EXISTS ${LICENSE_FILE})
    configure_file(${LICENSE_FILE}
            ${CMAKE_CURRENT_BINARY_DIR}/input
            COPYONLY)
else()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/input
            "testingabcabcabc123\n\nhi\n123")
endif()

add_custom_target(run
        COMMAND $<TARGET_FILE:odz> c input output.odz
        COMMAND $<TARGET_FILE:odz> d output.odz roundtrip
        COMMAND ${CMAKE_COMMAND} -E compare_files input roundtrip
        DEPENDS odz
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Compress → Decompress → Compare (input vs roundtrip)"
)

add_custom_target(tidy
        COMMAND ${CMAKE_COMMAND} -E rm -f output.odz roundtrip
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Remove generated output files"
)

install(TARGETS odz RUNTIME DESTINATION bin)
