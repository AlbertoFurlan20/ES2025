project(sensor_driver)

# GLOB_RECURSE, not GLOB: sources live one level down, in a folder per module
# (bmp180, i2c, bmp_app, telemetry). init.cpp stays at the top because it
# belongs to none of them.
file(GLOB_RECURSE CPP_SRC src/*.cpp)
file(GLOB_RECURSE INCLUDE inc/*.h)

add_executable(sensor_test.elf ${C_SRC} ${CPP_SRC} ${INCLUDE})

# Only inc/ goes on the path, never inc/<module>/. That is what makes an include
# spell out which module it is reaching into: "bmp_app/app.h", not "app.h".
target_include_directories(sensor_test.elf PRIVATE inc)

add_custom_command(TARGET sensor_test.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary sensor_test.elf sensor_test.bin
        COMMENT "Generating sensor_test.bin"
)