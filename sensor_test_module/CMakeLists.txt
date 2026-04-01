project(alive_driver C)

file(GLOB SRCS src/*.c)

file(GLOB INCLUDE inc/*.c)

add_executable(sensor_test.elf ${SRCS} ${INCLUDE})

target_include_directories(sensor_test.elf PRIVATE inc)

# Post-build: generate .bin for flashing
add_custom_command(TARGET sensor_test.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary sensor_test.elf sensor_test.bin
        COMMENT "Generating sensor_test.bin"
)