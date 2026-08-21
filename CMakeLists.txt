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

add_subdirectory(protocol)
add_subdirectory(rt)
add_subdirectory(ipc)
add_subdirectory(tools/valet_probe)
add_subdirectory(tests)
