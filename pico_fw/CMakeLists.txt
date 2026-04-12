cmake_minimum_required(VERSION 3.12)

set(PICO_PLATFORM rp2350)
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

include(pico_extras_import_optional.cmake)

project(xbox_jewel C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(default_build_type "Release")

pico_sdk_init()

add_executable(xbox_jewel
        ${CMAKE_CURRENT_LIST_DIR}/src/xbox_jewel.c
)

target_include_directories(xbox_jewel PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/include
)

# pull in common dependencies
target_link_libraries(xbox_jewel
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_exception
        pico_sync
)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(xbox_jewel)
