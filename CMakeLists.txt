cmake_minimum_required(VERSION 3.16)
project(odzip C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ODZ_PORTABLE "Build portable binary (no -march=native)" OFF)

find_package(Threads REQUIRED)

set(LIB_SOURCES
    odz_util.c bitstream.c huffman.c lz_hashchain.c compress.c decompress.c
)

# Static library
add_library(odzip_static STATIC ${LIB_SOURCES})
set_target_properties(odzip_static PROPERTIES OUTPUT_NAME odzip)
target_include_directories(odzip_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(odzip_static PUBLIC Threads::Threads)

# Shared library
add_library(odzip_shared SHARED ${LIB_SOURCES})
set_target_properties(odzip_shared PROPERTIES OUTPUT_NAME odzip)
target_compile_options(odzip_shared PRIVATE -fPIC)
target_include_directories(odzip_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(odzip_shared PUBLIC Threads::Threads)

# CLI links against static library
add_executable(odz main.c)
target_link_libraries(odz PRIVATE odzip_static)

# Compiler flags
if (MSVC)
    target_compile_options(odzip_static PRIVATE /W4)
    target_compile_options(odzip_shared PRIVATE /W4)
    target_compile_options(odz PRIVATE /W4)
else()
    set(COMMON_FLAGS -O2 -Wall -Wextra -pedantic -flto)
    if (NOT ODZ_PORTABLE)
        list(APPEND COMMON_FLAGS -march=native)
    endif()
    target_compile_options(odzip_static PRIVATE ${COMMON_FLAGS})
    target_compile_options(odzip_shared PRIVATE ${COMMON_FLAGS})
    target_compile_options(odz PRIVATE ${COMMON_FLAGS})
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
        COMMAND $<TARGET_FILE:odz> -f -v0 c input output.odz
        COMMAND $<TARGET_FILE:odz> -f -v0 d output.odz roundtrip
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

install(TARGETS odzip_static odzip_shared ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(FILES libodzip.h DESTINATION include)
install(TARGETS odz RUNTIME DESTINATION bin)
install(CODE "execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink odz \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/odzip)")
