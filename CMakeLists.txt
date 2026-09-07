cmake_minimum_required(VERSION 3.20)

# 1. System configuration
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 2. Define exactly where the compiler is located BEFORE project()
# The toolchain path is written in exactly one file, .env/setup.env, which
# C_src/compile.sh, C_src/flash.sh and C_src/Makefile read too. First time: copy
# .env/setup.env.example to .env/setup.env and edit the path.
#
# Precedence: -DRTEMS_LOCAL_PATH on the command line, then the environment, then
# the file. Read with file(STRINGS) rather than include() so the same plain
# KEY=VALUE text serves CMake and the shell without either owning the format.
set(ES_ENV_FILE ${CMAKE_SOURCE_DIR}/.env/setup.env)

if(NOT DEFINED RTEMS_LOCAL_PATH)
  if(DEFINED ENV{RTEMS_LOCAL_PATH})
    set(RTEMS_LOCAL_PATH $ENV{RTEMS_LOCAL_PATH})
  elseif(EXISTS ${ES_ENV_FILE})
    # A later assignment wins, so an appended line overrides an earlier one the
    # way it would in a shell.
    file(STRINGS ${ES_ENV_FILE} ES_ENV_LINES REGEX "^[ \t]*RTEMS_LOCAL_PATH[ \t]*=")
    if(ES_ENV_LINES)
      list(POP_BACK ES_ENV_LINES ES_ENV_LINE)
      string(REGEX REPLACE "^[ \t]*RTEMS_LOCAL_PATH[ \t]*=[ \t]*" "" RTEMS_LOCAL_PATH "${ES_ENV_LINE}")
      string(REGEX REPLACE "[\"\r]" "" RTEMS_LOCAL_PATH "${RTEMS_LOCAL_PATH}")
      string(STRIP "${RTEMS_LOCAL_PATH}" RTEMS_LOCAL_PATH)
    endif()
  endif()
endif()

if(NOT RTEMS_LOCAL_PATH)
  message(FATAL_ERROR
      "Missing RTEMS_LOCAL_PATH. Copy .env/setup.env.example to .env/setup.env "
      "and set your toolchain path.")
endif()

set(RTEMS_PREFIX       ${RTEMS_LOCAL_PATH}/RTEMS_toolchain/rtems)
set(CMAKE_C_COMPILER   ${RTEMS_PREFIX}/bin/arm-rtems7-gcc)
set(CMAKE_CXX_COMPILER ${RTEMS_PREFIX}/bin/arm-rtems7-g++)
set(CMAKE_OBJCOPY      ${RTEMS_PREFIX}/bin/arm-rtems7-objcopy)

# Suppress compiler checks (bare metal)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Set C++17 standard and ensure it's required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 3. NOW call project() so it initializes with the compilers specified above
project(ES2025 CXX)

# 4. Next, setup RTEMS specific flags and includes
set(RTEMS_BSP_LIB ${RTEMS_PREFIX}/7/arm-rtems7/stm32f4/lib)
set(RTEMS_INCLUDE ${RTEMS_PREFIX}/7/arm-rtems7/stm32f4/lib/include)
set(RTEMS_BASE_INCLUDE ${RTEMS_PREFIX}/arm-rtems7/include)

set(RTEMS_FLAGS
        -mcpu=cortex-m4
        -mthumb
        -mfloat-abi=hard
        -mfpu=fpv4-sp-d16
        -O0 -g
        # -Werror since v1.5.0: the warning list has been empty since R1 fixed
        # I2, so anything new here is a regression and should stop the build
        # rather than scroll past.
        -Wall -Wextra -Werror
        # No throw, try or catch anywhere in the tree since bmp180_task_manual
        # was deleted, so the unwind tables are dead weight.
        -fno-exceptions
        -fno-rtti
        -B${RTEMS_BSP_LIB}/
        -qrtems
)

add_compile_options(${RTEMS_FLAGS})

# Add both INCLUDE and BASE_INCLUDE
include_directories(
        ${RTEMS_INCLUDE}
        ${RTEMS_BASE_INCLUDE}
)

# Add each module as a subdirectory
add_subdirectory("C_src")
