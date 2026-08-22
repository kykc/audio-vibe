# yaml-cpp, fetched as a pinned release archive -- the session file's parser and emitter.
#
# Why a source dependency rather than the conda-forge package: conda-forge ships yaml-cpp as a
# DLL, and a DLL is a file the installer has to carry and a way for the shell to fail to start
# with no diagnostic (status.md sec. 8 item 21). Built from source it is a static library inside
# our own binaries, which is also what AGENTS.md asks for -- third-party source deps come in
# through FetchContent only.
#
# Refreshing the pin: the tags are at https://github.com/jbeder/yaml-cpp/tags; download the
# archive, `sha256sum` it, and update both values here.

include_guard(GLOBAL)
include(FetchContent)

set(AIP_YAML_CPP_VERSION "0.8.0")
set(AIP_YAML_CPP_URL
    "https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz")
set(AIP_YAML_CPP_SHA256 "fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16")

# Read as `option()`s in yaml-cpp's own scope, and unlike the SDK's this project does *not* get
# CMP0077 for free: its version floor is below 3.10, so `option()` reverts to clearing any normal
# variable of the same name and every setting below is silently discarded -- with the tools,
# contrib and clang-format targets switched back on. CMake says so, in a warning that names the
# variable it just threw away. Forcing the policy NEW for the subproject is what makes these
# lines mean anything; they stay normal variables rather than cache entries, as in vst3sdk.cmake.
set(YAML_CPP_BUILD_TESTS OFF)
set(YAML_CPP_BUILD_TOOLS OFF)
set(YAML_CPP_BUILD_CONTRIB OFF)
set(YAML_CPP_INSTALL OFF)
# Adds a `format` target that shells out to clang-format if it can find one. We have no such
# target and do not want one appearing from a dependency.
set(YAML_CPP_FORMAT_SOURCE OFF)
# Static, so nothing has to be shipped next to the executable. YAML_MSVC_SHARED_RT stays at its
# default ON, which means /MD -- the runtime everything else here is built against (sec. 6.4).
set(YAML_BUILD_SHARED_LIBS OFF)

# yaml-cpp 0.8.0 declares `cmake_minimum_required(VERSION 3.4)`, and CMake 4 removed
# compatibility with anything below 3.5 -- so without this the configure fails inside the
# dependency, before a single line of it is read, with an error that names only its version
# floor. 0.8.0 is the newest release (2023) and there is no fixed tag to move to. This is the
# documented escape hatch. It is set around the fetch and restored afterwards rather than left
# on, so it applies to this dependency and not to anything we might add later.
set(AIP_SAVED_POLICY_VERSION_MINIMUM "${CMAKE_POLICY_VERSION_MINIMUM}")
set(AIP_SAVED_POLICY_DEFAULT_CMP0077 "${CMAKE_POLICY_DEFAULT_CMP0077}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

FetchContent_Declare(yamlcpp
    URL "${AIP_YAML_CPP_URL}"
    URL_HASH "SHA256=${AIP_YAML_CPP_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    # yaml-cpp's headers are not clean under our /W4 /permissive-.
    SYSTEM
)

FetchContent_MakeAvailable(yamlcpp)

set(CMAKE_POLICY_VERSION_MINIMUM "${AIP_SAVED_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_DEFAULT_CMP0077 "${AIP_SAVED_POLICY_DEFAULT_CMP0077}")
unset(AIP_SAVED_POLICY_VERSION_MINIMUM)
unset(AIP_SAVED_POLICY_DEFAULT_CMP0077)

if(NOT TARGET yaml-cpp::yaml-cpp)
    message(FATAL_ERROR "yaml-cpp populated but yaml-cpp::yaml-cpp does not exist.")
endif()
