cmake_minimum_required(VERSION 3.25)

add_library(atriswave_shared INTERFACE)
add_library(atriswave::shared ALIAS atriswave_shared)

target_include_directories(atriswave_shared
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(atriswave_shared INTERFACE cxx_std_20)
