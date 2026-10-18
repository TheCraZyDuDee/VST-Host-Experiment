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

set(VST3_SDK_ROOT "" CACHE PATH "Path to a recursively cloned Steinberg VST3 SDK (optional)")
if(VST3_SDK_ROOT)
    if(CMAKE_VERSION VERSION_LESS 3.25)
        message(FATAL_ERROR "The VST3 SDK requires CMake 3.25 or newer. Install a newer CMake or leave VST3_SDK_ROOT empty.")
    endif()
    if(NOT EXISTS "${VST3_SDK_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR "VST3_SDK_ROOT must point to the root of the Steinberg vst3sdk checkout.")
    endif()

    # The SDK's examples and VSTGUI are not needed by this headless host.
    set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
    add_subdirectory("${VST3_SDK_ROOT}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    if(NOT TARGET sdk_hosting)
        message(FATAL_ERROR "The selected VST3 SDK did not define sdk_hosting. Ensure the SDK checkout includes its recursive submodules.")
    endif()
    # PlugProvider is a host helper used by the SDK samples but is not part of
    # sdk_hosting in every SDK revision, so compile it into the host target.
    target_sources(sdk_hosting PRIVATE "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp")
    set(VST_HOST_HAVE_VST3 ON)
else()
    set(VST_HOST_HAVE_VST3 OFF)
    message(STATUS "VST3 support disabled. Set VST3_SDK_ROOT to enable --plugin.")
endif()

add_executable(vst-host
    src/main.cpp
    src/audio/pipewire_engine.cpp
    src/audio/gain_processor.cpp
    src/audio/vst3_processor.cpp
)

target_compile_features(vst-host PRIVATE cxx_std_20)
target_link_libraries(vst-host PRIVATE PkgConfig::PIPEWIRE)

if(VST_HOST_HAVE_VST3)
    target_compile_definitions(vst-host PRIVATE VST_HOST_ENABLE_VST3=1)
    target_link_libraries(vst-host PRIVATE sdk_hosting)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(ring-buffer-test tests/ring_buffer_test.cpp)
    target_include_directories(ring-buffer-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(ring-buffer-test PRIVATE cxx_std_20)
    add_test(NAME ring-buffer-test COMMAND ring-buffer-test)

    add_executable(audio-processor-test tests/audio_processor_test.cpp src/audio/gain_processor.cpp)
    target_include_directories(audio-processor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(audio-processor-test PRIVATE cxx_std_20)
    add_test(NAME audio-processor-test COMMAND audio-processor-test)
endif()

if(MSVC)
    target_compile_options(vst-host PRIVATE /W4 /permissive-)
else()
    target_compile_options(vst-host PRIVATE -Wall -Wextra -Wpedantic)
endif()
