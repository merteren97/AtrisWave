cmake_minimum_required(VERSION 3.25)

project(AtrisWavePluginHost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../shared" "${CMAKE_CURRENT_BINARY_DIR}/shared")

add_library(atriswave_plugin_sandbox_client INTERFACE)
target_include_directories(atriswave_plugin_sandbox_client
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(atriswave_plugin_sandbox_client
    INTERFACE
        atriswave::shared
        advapi32.lib
)
target_compile_definitions(atriswave_plugin_sandbox_client
    INTERFACE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)
if(MSVC)
    target_compile_options(atriswave_plugin_sandbox_client
        INTERFACE
            /W4
            /WX
            /permissive-
            /EHsc
    )
endif()

add_executable(AtrisWavePluginHost
    src/host_main.cpp
)
target_link_libraries(AtrisWavePluginHost
    PRIVATE
        atriswave::shared
        avrt.lib
        advapi32.lib
)
target_compile_definitions(AtrisWavePluginHost
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)
if(MSVC)
    target_compile_options(AtrisWavePluginHost
        PRIVATE
            /W4
            /WX
            /permissive-
            /EHsc
    )
endif()

add_library(atriswave_sample_gain_plugin SHARED
    src/sample_gain_plugin.cpp
)
target_link_libraries(atriswave_sample_gain_plugin
    PRIVATE
        atriswave::shared
)
target_compile_definitions(atriswave_sample_gain_plugin
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)
if(MSVC)
    target_compile_options(atriswave_sample_gain_plugin
        PRIVATE
            /W4
            /WX
            /permissive-
            /EHsc
    )
endif()

add_executable(atriswave_plugin_host_tests
    tests/plugin_host_tests.cpp
)
target_link_libraries(atriswave_plugin_host_tests
    PRIVATE
        atriswave_plugin_sandbox_client
        atriswave::shared
)
target_compile_definitions(atriswave_plugin_host_tests
    PRIVATE
        ATRISWAVE_PLUGIN_HOST_EXE="$<TARGET_FILE:AtrisWavePluginHost>"
        ATRISWAVE_SAMPLE_PLUGIN_DLL="$<TARGET_FILE:atriswave_sample_gain_plugin>"
)

add_test(
    NAME atriswave_plugin_host_tests
    COMMAND atriswave_plugin_host_tests
)
