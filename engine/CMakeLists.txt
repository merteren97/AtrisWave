cmake_minimum_required(VERSION 3.25)

project(AtrisWaveEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared" "${CMAKE_CURRENT_BINARY_DIR}/shared")

add_library(atriswave_engine_core STATIC
    src/telemetry_publisher.cpp
    src/control_server.cpp
)

target_include_directories(atriswave_engine_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(atriswave_engine_core
    PUBLIC
        atriswave::shared
        avrt.lib
        advapi32.lib
)

target_compile_definitions(atriswave_engine_core
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(atriswave_engine_core
        PUBLIC
            /W4
            /WX
            /permissive-
            /EHsc
    )
endif()

add_executable(AtrisWaveEngine
    src/main.cpp
)

target_link_libraries(AtrisWaveEngine
    PRIVATE
        atriswave_engine_core
)

add_executable(atriswave_engine_tests
    tests/engine_tests.cpp
)

target_link_libraries(atriswave_engine_tests
    PRIVATE
        atriswave_engine_core
)

add_test(
    NAME atriswave_engine_tests
    COMMAND atriswave_engine_tests
)
