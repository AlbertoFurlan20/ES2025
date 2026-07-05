cmake_minimum_required(VERSION 3.20)
project(alive_driver C CXX)

set(SRCS
        init.c
        alive.c
)

add_executable(alive.elf ${SRCS})

# Post-build: generate .bin for flashing
add_custom_command(TARGET alive.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary alive.elf alive.bin
        COMMENT "Generating alive.bin"
)