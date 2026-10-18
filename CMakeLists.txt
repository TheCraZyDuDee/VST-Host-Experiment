cmake_minimum_required(VERSION 3.20)

project(vst-host
    VERSION 0.2.0
    DESCRIPTION "Lightweight Linux-first real-time audio plugin host"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_executable(vst-host
    src/main.cpp
    src/audio/pipewire_engine.cpp
)

target_compile_features(vst-host PRIVATE cxx_std_20)
target_link_libraries(vst-host PRIVATE PkgConfig::PIPEWIRE)

include(CTest)
if(BUILD_TESTING)
    add_executable(ring-buffer-test tests/ring_buffer_test.cpp)
    target_include_directories(ring-buffer-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(ring-buffer-test PRIVATE cxx_std_20)
    add_test(NAME ring-buffer-test COMMAND ring-buffer-test)
endif()

if(MSVC)
    target_compile_options(vst-host PRIVATE /W4 /permissive-)
else()
    target_compile_options(vst-host PRIVATE -Wall -Wextra -Wpedantic)
endif()
