# Script-mode half of cmake/version.cmake -- see the comments there for why this runs on every
# build rather than once at configure time. Invoked as `cmake -P`, so it has no project, no
# targets and no cache; everything it needs arrives as `-D`.

if(NOT DEFINED AIP_VERSION_HEADER OR NOT DEFINED AIP_VERSION_SOURCE_DIR)
    message(FATAL_ERROR "write_version_header.cmake needs AIP_VERSION_HEADER and AIP_VERSION_SOURCE_DIR")
endif()

set(description "unknown")

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${AIP_VERSION_SOURCE_DIR}"
        OUTPUT_VARIABLE hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE hashStatus)

    if(hashStatus EQUAL 0 AND NOT hash STREQUAL "")
        set(description "${hash}")

        # Tracked files only. An untracked scratch file in the tree is not a different build.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain -uno
            WORKING_DIRECTORY "${AIP_VERSION_SOURCE_DIR}"
            OUTPUT_VARIABLE modifications
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE modificationsStatus)

        if(modificationsStatus EQUAL 0 AND NOT modifications STREQUAL "")
            set(description "${description}-dirty")
        endif()
    endif()
endif()

set(content
"// Generated on every build by cmake/write_version_header.cmake. It lives in the build tree; do
// not edit it and do not commit it.

#pragma once

namespace aip {

/// The project version, straight from `project()` in the top-level CMakeLists.txt.
inline constexpr const char* kVersionNumber = \"${AIP_VERSION_NUMBER}\";

/// The commit this binary was built from, abbreviated, with `-dirty` appended when tracked files
/// differed from it. `unknown` when the source tree is not a git repository or git could not be
/// run -- an exported archive is a real case and is not a build failure.
inline constexpr const char* kGitDescription = \"${description}\";

} // namespace aip
")

# Written beside the target and moved into place only when it differs, so an unchanged commit does
# not retrigger every compilation that includes this.
file(WRITE "${AIP_VERSION_HEADER}.tmp" "${content}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${AIP_VERSION_HEADER}.tmp" "${AIP_VERSION_HEADER}")
