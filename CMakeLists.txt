cmake_minimum_required(VERSION 3.20)

# 1. System configuration
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 2. Define exactly where the compiler is located BEFORE project()
# Each developer sets RTEMS_PREFIX once in local.cmake (gitignored).
# First time: copy local.cmake.example -> local.cmake and edit the path.
include(${CMAKE_SOURCE_DIR}/local.cmake OPTIONAL)
if(NOT DEFINED RTEMS_LOCAL_PATH)
  message(FATAL_ERROR "Missing RTEMS_LOCAL_PATH. Copy local.cmake.example to local.cmake and set your toolchain path.")
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
add_subdirectory("communication_test_module")
add_subdirectory("sensor_module")
