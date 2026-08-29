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
            -D "AIP_VERSION_NUMBER=${PROJECT_VERSION}"
            -D "GIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${CMAKE_CURRENT_LIST_DIR}/write_version_header.cmake"
    COMMENT "Resolving the commit this build comes from"
    VERBATIM)

# No BYPRODUCTS and no ALL, both on purpose. BYPRODUCTS would declare the same output twice under
# a cross-config build; ordering through `add_dependencies` is what the consumer actually needs,
# and it keeps the git calls out of builds of targets that do not want the header.
