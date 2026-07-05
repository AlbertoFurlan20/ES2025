cmake_minimum_required(VERSION 3.20)
project(alive_driver C CXX)

set(SRCS
        init.c
        alive.c
        ../sensor_test_module/src/bmp180.cpp
        ../sensor_test_module/inc/bmp.h
        ../sensor_test_module/inc/bmp180_ioctls.h
        ../sensor_test_module/inc/bmp_regs.h
        ../sensor_test_module/inc/constants.h
        ../sensor_test_module/inc/i2c1.h
        ../sensor_test_module/src/alive.cpp
        ../sensor_test_module/src/i2c1.cpp
        ../sensor_test_module/src/init.cpp
        ../sensor_test_module/src/sensor.cpp
)

add_executable(alive.elf ${SRCS})

# Post-build: generate .bin for flashing
add_custom_command(TARGET alive.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary alive.elf alive.bin
        COMMENT "Generating alive.bin"
)