cmake_minimum_required(VERSION 3.20)

project(vst-host
    VERSION 0.1.0
    DESCRIPTION "Lightweight Linux-first real-time audio plugin host"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(vst-host src/main.cpp)
target_compile_features(vst-host PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(vst-host PRIVATE /W4 /permissive-)
else()
    target_compile_options(vst-host PRIVATE -Wall -Wextra -Wpedantic)
endif()
