# The version the About box shows: the project version from `project()`, and the commit the
# binary was actually built from.
#
# The commit is resolved at **build** time, not configure time, and that is the whole reason this
# is a custom target rather than three lines of `execute_process` up in the top-level file. A
# configure-time hash is captured once and then goes stale the moment you commit and rebuild --
# leaving an About box that confidently names the previous commit, which is worse than naming
# none. The only question this string exists to answer is "which build is the user running", so a
# stale answer is a defect rather than an inaccuracy.
#
# `-uno` on the dirty check is deliberate: dirty means *tracked files differ from the commit*. A
# scratch file sitting in the working tree is not a different build and should not be reported as
# one.
#
# Everything degrades to `unknown` rather than failing: no git on PATH, not a repository at all
# (an exported archive builds perfectly well), or a git that errors for its own reasons.
#
# The header is written per configuration. Ninja Multi-Config declares this target's rules once
# per config against one binary directory, and two configs writing one path is both a race and
# the shape of failure that `CMAKE_CXX_SCAN_FOR_MODULES OFF` exists to avoid at the top level. A
# few hundred duplicated bytes is the cheap way out of it.

include_guard(GLOBAL)

find_package(Git QUIET)

# Consumers add this to their include path and `#include "aip/version.h"`. The generator
# expression is why it cannot simply be a plain path.
set(AIP_VERSION_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated/version/$<CONFIG>")

add_custom_target(aip_version_header
    COMMAND ${CMAKE_COMMAND}
            -D "AIP_VERSION_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -D "AIP_VERSION_HEADER=${AIP_VERSION_INCLUDE_DIR}/aip/version.h"
            -D "AIP_VERSION_RC_HEADER=${AIP_VERSION_INCLUDE_DIR}/aip/version_rc.h"
            -D "AIP_VERSION_NUMBER=${PROJECT_VERSION}"
            -D "AIP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
            -D "AIP_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
            -D "AIP_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
            -D "GIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${CMAKE_CURRENT_LIST_DIR}/write_version_header.cmake"
    COMMENT "Resolving the commit this build comes from"
    VERBATIM)

# No BYPRODUCTS and no ALL, both on purpose. BYPRODUCTS would declare the same output twice under
# a cross-config build; ordering through `add_dependencies` is what the consumer actually needs,
# and it keeps the git calls out of builds of targets that do not want the header.

# Fixed strings for the resource. Here rather than repeated at five call sites, and taken from the
# two places that already own them: the product name is what the shell calls itself everywhere the
# user sees it, and the copyright is the LICENSE file's, verbatim.
set(AIP_RESOURCE_COMPANY "Alexander Prokopchuk")
set(AIP_RESOURCE_PRODUCT "VibeAudio")
set(AIP_RESOURCE_COPYRIGHT "Copyright (c) 2026 Alexander Prokopchuk. MIT License.")

# Gives `target` a VERSIONINFO resource, so that Explorer's Details tab, Task Manager and anything
# else that reads file metadata can say which build this is.
#
# Applied to every binary the package ships and to nothing else. A development-only tool with no
# version resource is a file nobody is ever asked about; one that shipped without it is a support
# conversation with no way to start.
#
# `DESCRIPTION` is the FileDescription string, which is the one a person actually reads -- it is
# the Description column in Task Manager and in Explorer's Details view. Everything else is
# derived: the file name from the target's own output name, and VFT_APP against VFT_DLL from its
# type, because a resource that disagreed with either would be a resource somebody hand-edited
# and forgot.
function(aip_add_version_resource target)
    cmake_parse_arguments(arg "" "DESCRIPTION" "" ${ARGN})
    if(NOT arg_DESCRIPTION)
        message(FATAL_ERROR "aip_add_version_resource(${target}) needs DESCRIPTION")
    endif()

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        set(AIP_RESOURCE_FILETYPE "VFT_APP")
        set(suffix "${CMAKE_EXECUTABLE_SUFFIX}")
    elseif(type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
        set(AIP_RESOURCE_FILETYPE "VFT_DLL")
        set(suffix "${CMAKE_SHARED_LIBRARY_SUFFIX}")
    else()
        message(FATAL_ERROR "aip_add_version_resource(${target}): ${type} carries no version resource")
    endif()

    # OUTPUT_NAME when the target sets one -- aip_apo does -- and the target's own name otherwise,
    # which is how the rest of them get theirs.
    get_target_property(outputName ${target} OUTPUT_NAME)
    if(NOT outputName)
        set(outputName "${target}")
    endif()

    set(AIP_RESOURCE_DESCRIPTION "${arg_DESCRIPTION}")
    set(AIP_RESOURCE_INTERNAL_NAME "${outputName}")
    set(AIP_RESOURCE_ORIGINAL_FILENAME "${outputName}${suffix}")

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/version_resource.rc.in" "${generated}" @ONLY)
    target_sources(${target} PRIVATE "${generated}")

    # The generated `aip/version_rc.h` the script above includes, and the ordering that makes sure
    # it is there before rc.exe looks for it. Both are needed even on a target that already links
    # nothing else of ours -- aip_apo is one.
    target_include_directories(${target} PRIVATE ${AIP_VERSION_INCLUDE_DIR})
    add_dependencies(${target} aip_version_header)
endfunction()
