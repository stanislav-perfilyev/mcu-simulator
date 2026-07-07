cmake_minimum_required(VERSION 3.20)
project(mcu_simulator VERSION 1.0.0 LANGUAGES CXX)

# ─── Global settings ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings
add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Werror
    -Wno-unused-parameter
    $<$<CXX_COMPILER_ID:Clang>:-Wno-gnu-zero-variadic-macro-arguments>
)

# ─── Core library ─────────────────────────────────────────────────────────────
add_library(sim_core STATIC
    src/cpu.cpp
    src/memory.cpp
    src/loader.cpp
)
target_include_directories(sim_core PUBLIC src)
target_compile_features(sim_core PUBLIC cxx_std_20)

# ─── Simulator executable ─────────────────────────────────────────────────────
add_executable(simulator src/main.cpp)
target_link_libraries(simulator PRIVATE sim_core)

# ─── Google Test ──────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(test_cpu     tests/test_cpu.cpp)
add_executable(test_memory  tests/test_memory.cpp)
add_executable(test_loader  tests/test_loader.cpp)

target_link_libraries(test_cpu    PRIVATE sim_core GTest::gtest_main)
target_link_libraries(test_memory PRIVATE sim_core GTest::gtest_main)
target_link_libraries(test_loader PRIVATE sim_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(test_cpu)
gtest_discover_tests(test_memory)
gtest_discover_tests(test_loader)

# ─── RTOS kernel library ──────────────────────────────────────────────────────
add_library(rtos_core STATIC
    rtos/scheduler.cpp
)
target_include_directories(rtos_core PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(rtos_core PUBLIC cxx_std_20)

# ─── RTOS tests ───────────────────────────────────────────────────────────────
add_executable(test_rtos tests/test_rtos.cpp)
target_link_libraries(test_rtos PRIVATE rtos_core GTest::gtest_main)
gtest_discover_tests(test_rtos)

# ─── RTOS demo (blinky) ───────────────────────────────────────────────────────
add_executable(blinky demo/blinky.cpp)
target_link_libraries(blinky PRIVATE rtos_core)

# ─── Peripheral library ───────────────────────────────────────────────────────
add_library(periph_core STATIC
    peripherals/uart.cpp
    peripherals/spi.cpp
    peripherals/i2c.cpp
    peripherals/can.cpp
)
target_include_directories(periph_core PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(periph_core PUBLIC cxx_std_20)

# ─── Peripheral tests ─────────────────────────────────────────────────────────
add_executable(test_peripherals tests/test_peripherals.cpp)
target_include_directories(test_peripherals PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_peripherals PRIVATE periph_core GTest::gtest_main)
gtest_discover_tests(test_peripherals)

# ─── Sensor pipeline demo ─────────────────────────────────────────────────────
add_executable(sensor_demo demo/sensor_demo.cpp)
target_include_directories(sensor_demo PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(sensor_demo PRIVATE periph_core rtos_core)

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS simulator blinky sensor_demo DESTINATION bin)
