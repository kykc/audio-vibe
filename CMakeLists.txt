cmake_minimum_required(VERSION 3.28)

project(audio_ipc2
    VERSION 0.1.0
    DESCRIPTION "System-wide audio processing utility"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project uses no C++20 modules and has no plans to. With C++20 declared, CMake otherwise
# emits a module-scanning (dyndep) step for every target in every configuration -- pure overhead
# here, and worse than overhead under Ninja Multi-Config with CMAKE_CROSS_CONFIGS=all: the
# per-object modmap of an OBJECT library gets declared once per configuration with the same output
# path, and ninja refuses to load the build file at all:
#
#   ninja: error: CMakeFiles/impl-Release.ninja:NNNN:
#   rt/CMakeFiles/aip_rt_hooks.dir/RelWithDebInfo/src/alloc_hooks.cpp.obj.modmap
#   is defined as an output multiple times
#
# That is what made `cmake --build --preset release` impossible while RelWithDebInfo built fine.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# RelWithDebInfo is the primary development configuration (design_doc.md sec. 6.4): conda-forge
# qt6-main ships no debug libraries, so a Debug configuration cannot link. Drop NDEBUG from it
# so that assert() and the real-time violation detector (sec. 7.4.6) are actually live in the
# configuration we develop and test in.
string(REPLACE "/DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

# Common compile settings for every first-party target.
add_library(aip_common INTERFACE)
target_compile_definitions(aip_common INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # The real-time violation detector is compiled into RelWithDebInfo, out of Release (sec. 7.4.6).
    $<$<CONFIG:RelWithDebInfo,Debug>:AIP_RT_CHECKS=1>
)
if(MSVC)
    target_compile_options(aip_common INTERFACE
        /W4 /permissive- /utf-8 /EHsc /Zc:preprocessor /Zc:__cplusplus /volatile:iso
        /diagnostics:caret)
endif()

enable_testing()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Third-party source dependencies come in through FetchContent only (sec. 6.1). The SDK is
# fetched at the top level so that engine/, scanner/ and tests/ all share one copy; yaml-cpp has
# a single consumer in config/, and is here so that both third-party pins are in one place.
include(vst3sdk)
include(yamlcpp)

# Not a third-party pin like the two above, but included alongside them for the same reason: it
# is a decision about what a number means, and it has to be made before any target that shows it.
include(version)

add_subdirectory(protocol)
add_subdirectory(rt)
add_subdirectory(ipc)
# The APO (sec. 7.1). Deliberately after ipc/ -- it compiles a few of that component's sources
# into itself rather than linking them, because it is the one target built /MT (apo/CMakeLists.txt).
add_subdirectory(apo)
add_subdirectory(engine)
add_subdirectory(scanner)
# After scanner/: aip_config links it for ScannedModule, which is part of what a session holds.
add_subdirectory(config)
add_subdirectory(ui)
add_subdirectory(tools/apo_admin)
add_subdirectory(tools/apo_host)
add_subdirectory(tools/valet_probe)
add_subdirectory(tools/editor_spike)
add_subdirectory(tests)

# After the subdirectories: it names aip_ui and aip_scan, and it reads QT6_INSTALL_PREFIX, which
# ui/'s find_package sets.
include(package)
