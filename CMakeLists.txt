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

option(VST_HOST_ENABLE_VST3 "Enable native VST3 hosting (fetch the SDK automatically if needed)" OFF)
option(VST_HOST_ENABLE_VST2 "Enable legacy VST2.4 hosting (requires a legally licensed, user-provided SDK)" OFF)
set(VST2_SDK_ROOT "" CACHE PATH "Local directory containing licensed VST2.4 aeffectx.h (never downloaded)")
if(VST_HOST_ENABLE_VST2)
    if(NOT VST2_SDK_ROOT)
        message(FATAL_ERROR "VST2 support requires -DVST2_SDK_ROOT=... pointing to your licensed legacy SDK. Steinberg stopped issuing VST2 licenses in October 2018.")
    endif()
    find_path(VST2_SDK_INCLUDE_DIR aeffectx.h
        PATHS "${VST2_SDK_ROOT}" "${VST2_SDK_ROOT}/pluginterfaces/vst2.x"
        NO_DEFAULT_PATH)
    if(NOT VST2_SDK_INCLUDE_DIR)
        message(FATAL_ERROR "Could not find aeffectx.h in VST2_SDK_ROOT or its pluginterfaces/vst2.x subfolder. VST2 SDK files are intentionally not bundled or fetched.")
    endif()
    message(STATUS "Optional VST2.4 hosting enabled with user-provided headers at ${VST2_SDK_INCLUDE_DIR}. Ensure your development and distribution comply with Steinberg's legacy license.")
endif()
option(VST_HOST_BUILD_GUI "Build the optional Qt 6 GUI (requires VST3 hosting)" OFF)
set(VST3_SDK_ROOT "" CACHE PATH "Optional path to an existing Steinberg VST3 SDK checkout")

if(VST_HOST_ENABLE_VST3 OR VST3_SDK_ROOT)
    if(CMAKE_VERSION VERSION_LESS 3.25)
        message(FATAL_ERROR "VST3 hosting requires CMake 3.25 or newer. Upgrade CMake or disable VST3 hosting.")
    endif()

    # Steinberg's SDK requires DEVELOPMENT, RELEASE, _DEBUG, or NDEBUG.
    # Single-config generators (such as Unix Makefiles) otherwise default to
    # an empty build type, so choose Release unless the user specified one.
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
        message(STATUS "No build type specified; defaulting to Release for VST3 SDK compatibility.")
    endif()

    if(VST3_SDK_ROOT)
        if(NOT EXISTS "${VST3_SDK_ROOT}/CMakeLists.txt")
            message(FATAL_ERROR "VST3_SDK_ROOT must point to the root of the Steinberg vst3sdk checkout.")
        endif()
        set(_vst3_sdk_source "${VST3_SDK_ROOT}")
        message(STATUS "Using existing VST3 SDK at: ${_vst3_sdk_source}")
        set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_vst3_sdk_source}" "${CMAKE_BINARY_DIR}/vst3sdk" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)

        # Pin the SDK revision so clean builds remain reproducible. Only the
        # core SDK submodules are fetched; VSTGUI is not needed by this host.
        set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)

        FetchContent_Declare(vst3sdk
            GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
            GIT_TAG v3.8.0_build_66
            GIT_SHALLOW TRUE
            GIT_SUBMODULES base pluginterfaces public.sdk cmake
            GIT_SUBMODULES_RECURSE TRUE
        )
        FetchContent_MakeAvailable(vst3sdk)
        set(_vst3_sdk_source "${vst3sdk_SOURCE_DIR}")
    endif()

    if(NOT TARGET sdk_hosting)
        message(FATAL_ERROR "The selected VST3 SDK did not define sdk_hosting. Check that its required submodules were fetched.")
    endif()

    # These platform-specific hosting and provider implementations are not
    # included in sdk_hosting's source list by this SDK revision.
    target_sources(sdk_hosting PRIVATE
        "${_vst3_sdk_source}/public.sdk/source/common/memorystream.cpp"
        "${_vst3_sdk_source}/public.sdk/source/vst/hosting/module_linux.cpp"
        "${_vst3_sdk_source}/public.sdk/source/vst/hosting/plugprovider.cpp"
    )
    set(VST_HOST_HAVE_VST3 ON)
else()
    set(VST_HOST_HAVE_VST3 OFF)
    message(STATUS "VST3 support disabled. Set -DVST_HOST_ENABLE_VST3=ON to fetch and build the SDK automatically.")
endif()

add_executable(vst-host
    src/main.cpp
    src/audio/pipewire_engine.cpp
    src/audio/gain_processor.cpp
    src/audio/vst3_processor.cpp
    src/audio/vst2_processor.cpp
)

target_compile_features(vst-host PRIVATE cxx_std_20)
target_link_libraries(vst-host PRIVATE PkgConfig::PIPEWIRE ${CMAKE_DL_LIBS})
if(VST_HOST_ENABLE_VST2)
    target_compile_definitions(vst-host PRIVATE VST_HOST_ENABLE_VST2=1)
    target_include_directories(vst-host PRIVATE "${VST2_SDK_INCLUDE_DIR}")
endif()

if(VST_HOST_HAVE_VST3)
    target_compile_definitions(vst-host PRIVATE VST_HOST_ENABLE_VST3=1)
    target_link_libraries(vst-host PRIVATE sdk_hosting)
endif()


if(VST_HOST_BUILD_GUI)
    if(NOT VST_HOST_HAVE_VST3)
        message(FATAL_ERROR "VST_HOST_BUILD_GUI requires -DVST_HOST_ENABLE_VST3=ON.")
    endif()

    find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)
    find_package(X11 REQUIRED)
    add_executable(vst-host-gui
        src/gui/main.cpp
        src/audio/pipewire_engine.cpp
        src/audio/gain_processor.cpp
        src/audio/vst3_processor.cpp
        src/audio/vst2_processor.cpp
    )
    target_include_directories(vst-host-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(vst-host-gui PRIVATE cxx_std_20)
    target_compile_definitions(vst-host-gui PRIVATE VST_HOST_ENABLE_VST3=1)
    target_link_libraries(vst-host-gui PRIVATE PkgConfig::PIPEWIRE Qt6::Widgets X11::X11 sdk_hosting ${CMAKE_DL_LIBS})
    if(VST_HOST_ENABLE_VST2)
        target_compile_definitions(vst-host-gui PRIVATE VST_HOST_ENABLE_VST2=1)
        target_include_directories(vst-host-gui PRIVATE "${VST2_SDK_INCLUDE_DIR}")
    endif()
    if(MSVC)
        target_compile_options(vst-host-gui PRIVATE /W4 /permissive-)
    else()
        target_compile_options(vst-host-gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(ring-buffer-test tests/ring_buffer_test.cpp)
    target_include_directories(ring-buffer-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(ring-buffer-test PRIVATE cxx_std_20)
    add_test(NAME ring-buffer-test COMMAND ring-buffer-test)

    add_executable(capture-target-policy-test tests/capture_target_policy_test.cpp)
    target_include_directories(capture-target-policy-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(capture-target-policy-test PRIVATE cxx_std_20)
    add_test(NAME capture-target-policy-test COMMAND capture-target-policy-test)

    add_executable(plugin-state-test tests/plugin_state_test.cpp)
    target_include_directories(plugin-state-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(plugin-state-test PRIVATE cxx_std_20)
    add_test(NAME plugin-state-test COMMAND plugin-state-test)

    add_executable(parameter-event-queue-test tests/parameter_event_queue_test.cpp)
    target_include_directories(parameter-event-queue-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(parameter-event-queue-test PRIVATE cxx_std_20)
    add_test(NAME parameter-event-queue-test COMMAND parameter-event-queue-test)

    add_executable(audio-processor-test tests/audio_processor_test.cpp src/audio/gain_processor.cpp)
    target_include_directories(audio-processor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(audio-processor-test PRIVATE cxx_std_20)
    add_test(NAME audio-processor-test COMMAND audio-processor-test)

    add_executable(config-test tests/config_test.cpp)
    target_include_directories(config-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(config-test PRIVATE cxx_std_20)
    add_test(NAME config-test COMMAND config-test)

    add_executable(plugin-discovery-test tests/plugin_discovery_test.cpp)
    target_include_directories(plugin-discovery-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(plugin-discovery-test PRIVATE cxx_std_20)
    add_test(NAME plugin-discovery-test COMMAND plugin-discovery-test)

    add_executable(windows-bridge-manager-test tests/windows_bridge_manager_test.cpp)
    target_include_directories(windows-bridge-manager-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(windows-bridge-manager-test PRIVATE cxx_std_20)
    add_test(NAME windows-bridge-manager-test COMMAND windows-bridge-manager-test)

    add_executable(processor-chain-test tests/processor_chain_test.cpp)
    target_include_directories(processor-chain-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(processor-chain-test PRIVATE cxx_std_20)
    add_test(NAME processor-chain-test COMMAND processor-chain-test)

    add_executable(vst2-processor-test tests/vst2_processor_test.cpp src/audio/vst2_processor.cpp)
    target_include_directories(vst2-processor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(vst2-processor-test PRIVATE cxx_std_20)
    target_link_libraries(vst2-processor-test PRIVATE ${CMAKE_DL_LIBS})
    if(VST_HOST_ENABLE_VST2)
        target_compile_definitions(vst2-processor-test PRIVATE VST_HOST_ENABLE_VST2=1)
        target_include_directories(vst2-processor-test PRIVATE "${VST2_SDK_INCLUDE_DIR}")
    endif()
    add_test(NAME vst2-processor-test COMMAND vst2-processor-test)

    add_executable(vst3-processor-test tests/vst3_processor_test.cpp src/audio/vst3_processor.cpp)
    target_include_directories(vst3-processor-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(vst3-processor-test PRIVATE cxx_std_20)
    add_test(NAME vst3-processor-test COMMAND vst3-processor-test)

    # CTest runs in Release on CI. Preserve assertions in test executables:
    # many existing tests use assert() for both setup and verification.
    if(NOT MSVC)
        foreach(test_target IN ITEMS
                ring-buffer-test capture-target-policy-test plugin-state-test parameter-event-queue-test audio-processor-test config-test
                plugin-discovery-test windows-bridge-manager-test processor-chain-test
                vst2-processor-test vst3-processor-test)
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endforeach()
    endif()
endif()

if(MSVC)
    target_compile_options(vst-host PRIVATE /W4 /permissive-)
else()
    target_compile_options(vst-host PRIVATE -Wall -Wextra -Wpedantic)
endif()
