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
    src/audio/wasapi_devices.cpp
    src/audio/wasapi_stream.cpp
    src/audio/process_loopback.cpp
    src/audio/app_router.cpp
    src/audio/config_store.cpp
    src/audio/routing_rules.cpp
    src/audio/driver_bridge.cpp
    src/audio/audio_engine.cpp
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
        ole32.lib
        uuid.lib
        propsys.lib
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

add_executable(atriswave_dsp_tests
    tests/dsp_tests.cpp
)

target_link_libraries(atriswave_dsp_tests
    PRIVATE
        atriswave_engine_core
)

add_test(
    NAME atriswave_dsp_tests
    COMMAND atriswave_dsp_tests
)

add_executable(atriswave_driver_bridge_tests
    tests/driver_bridge_tests.cpp
)

target_link_libraries(atriswave_driver_bridge_tests
    PRIVATE
        atriswave_engine_core
)

add_test(
    NAME atriswave_driver_bridge_tests
    COMMAND atriswave_driver_bridge_tests
)
