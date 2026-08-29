# Script-mode half of cmake/version.cmake -- see the comments there for why this runs on every
# build rather than once at configure time. Invoked as `cmake -P`, so it has no project, no
# targets and no cache; everything it needs arrives as `-D`.

if(NOT DEFINED AIP_VERSION_HEADER OR NOT DEFINED AIP_VERSION_SOURCE_DIR)
    message(FATAL_ERROR "write_version_header.cmake needs AIP_VERSION_HEADER and AIP_VERSION_SOURCE_DIR")
endif()
if(NOT DEFINED AIP_VERSION_RC_HEADER)
    message(FATAL_ERROR "write_version_header.cmake needs AIP_VERSION_RC_HEADER")
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

# The one string both the About box and every binary's VERSIONINFO resource show, composed here
# and nowhere else. Two places that each assembled it from the number and the commit would be two
# places to keep in step, and the whole point of putting it in the resource is that a user reading
# Explorer and a user reading the About box are quoting the same thing.
set(versionString "${AIP_VERSION_NUMBER} (${description})")

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

/// The two above in the one form a user is ever asked to read back: what the About box shows, and
/// -- byte for byte -- what every binary's VERSIONINFO resource carries as FileVersion. Composed
/// in the generator so that Explorer and the dialog cannot disagree.
inline constexpr const char* kVersionString = \"${versionString}\";

} // namespace aip
")

# The same three facts as macros, for the resource compiler.
#
# A separate file rather than one both can read: rc.exe runs a C preprocessor over what it is
# given and then parses the result as resource script, so a header with a namespace and
# `inline constexpr` in it is a syntax error rather than a set of values. No `#pragma once`
# either, for the same reason -- an include guard is the form rc.exe understands.
#
# Three numbers and not four: VERSIONINFO's numeric field is four wide and `project()` gives
# three, so the fourth is a literal 0 in the template rather than a value invented here.
set(rcContent
"// Generated on every build by cmake/write_version_header.cmake, for cmake/version_resource.rc.in.
// It lives in the build tree; do not edit it and do not commit it.

#ifndef AIP_VERSION_RC_H
#define AIP_VERSION_RC_H

#define AIP_VERSION_MAJOR ${AIP_VERSION_MAJOR}
#define AIP_VERSION_MINOR ${AIP_VERSION_MINOR}
#define AIP_VERSION_PATCH ${AIP_VERSION_PATCH}
#define AIP_VERSION_STRING \"${versionString}\"

#endif
")

# Written beside the target and moved into place only when it differs, so an unchanged commit does
# not retrigger every compilation that includes this -- nor, for the second file, a resource
# compile and a relink of all five shipped binaries.
file(WRITE "${AIP_VERSION_HEADER}.tmp" "${content}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${AIP_VERSION_HEADER}.tmp" "${AIP_VERSION_HEADER}")

file(WRITE "${AIP_VERSION_RC_HEADER}.tmp" "${rcContent}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${AIP_VERSION_RC_HEADER}.tmp" "${AIP_VERSION_RC_HEADER}")
