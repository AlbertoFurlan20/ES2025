project(sensor_driver)

file(GLOB CPP_SRC src/*.cpp)
file(GLOB INCLUDE inc/*.h)

add_executable(sensor_test.elf ${C_SRC} ${CPP_SRC} ${INCLUDE}
        inc/telem_fmt.h)

# This was added just to directly include inc folder
# - i don't like relative imports
target_include_directories(sensor_test.elf PRIVATE inc)

add_custom_command(TARGET sensor_test.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary sensor_test.elf sensor_test.bin
        COMMENT "Generating sensor_test.bin"
)