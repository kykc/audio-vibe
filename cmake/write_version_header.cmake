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
set(committed "")

if(GIT_EXECUTABLE)
    # `--short=10` and not a bare `--short`. A bare one is git's `core.abbrev`, which is automatic
    # and grows with the object count, so the length is a property of the repository's size on the
    # day of the build. That is fine for a string a person reads and wrong for one a machine has to
    # reconstruct: `.gitea/workflows/publish-github-release.yaml` builds this same version out of a
    # commit, months later, with no clone to ask. Ten hex digits is deterministic and stays
    # unambiguous well past any size this repository will reach.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
        WORKING_DIRECTORY "${AIP_VERSION_SOURCE_DIR}"
        OUTPUT_VARIABLE hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE hashStatus)

    if(hashStatus EQUAL 0 AND NOT hash STREQUAL "")
        set(description "${hash}")

        # The committer date as epoch seconds, which is what orders the published version below.
        # `%ct` and not a formatted date on purpose: it is one integer, it has no timezone to
        # reproduce, and anything rebuilding this string elsewhere gets it from a commit's date
        # with one conversion and no formatting agreement to keep.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" show -s --format=%ct HEAD
            WORKING_DIRECTORY "${AIP_VERSION_SOURCE_DIR}"
            OUTPUT_VARIABLE committed
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE committedStatus)

        if(NOT committedStatus EQUAL 0)
            set(committed "")
        endif()

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

# The version the *package* is published under, which is a different string from the one above and
# deliberately so. It has to sort, and the one above does not: Scoop compares a pre-release suffix
# by splitting it into digit runs and letter runs and comparing the digit runs numerically, so
# `0.1.0-<hash>` orders by whatever number the hash happens to start with. `3313cac` beat
# `77fbaf9` -- 3313 against 77 -- and `scoop update` reported the newer build as already current
# and skipped it. Roughly half of all releases were coin-flip downgrades.
#
# Putting the committer epoch in front of the hash, with the hash after a `.` so it never
# participates in the ordering, fixes it: the leading digit run is then a timestamp and always
# ascends. It is the *commit's* time and not the build's, so one commit is one version however
# often it is rebuilt -- which is what lets a release be cut for a commit rather than for a build,
# and what keeps a re-run of a publish idempotent.
#
# Empty when the commit could not be resolved, which is the whole test the publishing step makes:
# `0.1.0-unknown` in a registry is a version nobody can trace back to a source tree. A dirty tree
# still publishes and still says so, because seeing that happen is the point of recording it.
set(packageVersion "")
if(NOT description STREQUAL "unknown" AND NOT committed STREQUAL "")
    set(packageVersion "${AIP_VERSION_NUMBER}-${committed}.${description}")
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

/// The two above in the one form a user is ever asked to read back: what the About box shows, and
/// -- byte for byte -- what every binary's VERSIONINFO resource carries as FileVersion. Composed
/// in the generator so that Explorer and the dialog cannot disagree.
inline constexpr const char* kVersionString = \"${versionString}\";

/// The string this build is published under: the project version, the committer date as epoch
/// seconds, and the commit -- `0.1.0-1756557590.d25a1070b0`. Unlike the two above it is built to
/// *sort*, because Scoop orders a pre-release suffix by its leading digit run and a bare hash has
/// no monotonic property under that rule. Empty when the commit could not be resolved, and the
/// workflow refuses to publish on that.
inline constexpr const char* kPackageVersion = \"${packageVersion}\";

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
