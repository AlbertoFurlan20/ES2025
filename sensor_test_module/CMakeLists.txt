project(alive_driver C)

set(SRCS
        alive.c
        init.c
        sensor.c
        constants.h
)

add_executable(sensor_test.elf ${SRCS})

# Post-build: generate .bin for flashing
add_custom_command(TARGET sensor_test.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary sensor_test.elf sensor_test.bin
        COMMENT "Generating sensor_test.bin"
)