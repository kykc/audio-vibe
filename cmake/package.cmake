# The portable package: a folder that runs `aip_ui.exe` on a machine that has none of this
# installed -- no pixi environment, no Qt, no Visual Studio.
#
# Three kinds of thing have to be in it, and only the first is found by looking at the executable:
#
#   imported DLLs    Qt6Core/Gui/Widgets and everything *they* import, which on a conda-forge Qt
#                    is a long tail of separate packages -- harfbuzz, freetype, pcre2, zlib, icu,
#                    double-conversion, md4c and so on. `file(GET_RUNTIME_DEPENDENCIES)` walks the
#                    import tables transitively and resolves each name against the environment.
#   Qt plugins       loaded by name at run time, so nothing imports them and no dependency walk
#                    can find them. Listed explicitly below, and then fed back into the walk,
#                    because a plugin has imports of its own.
#   the MSVC runtime the redistributable half of the compiler. Present on this machine because
#                    Visual Studio is; not present on a machine that only wants to run the thing.
#
# Not `windeployqt`. It is the canonical tool and it does not work here without help: the
# conda-forge layout has no `qtpaths` where it looks, so it fails with "Unable to query qtpaths"
# and copies nothing. Given `--qtpaths` explicitly it does run -- and then copies Qt's own DLLs
# and its plugins but none of the conda packages underneath them, which is exactly the half a
# dependency walk gets right and a Qt-aware tool does not. See status.md sec. 8 item 24.

include_guard(GLOBAL)

# Found again here, deliberately. `ui/` already does this, but a find_package in a subdirectory
# sets its variables in *that* scope -- so QT6_INSTALL_PREFIX is empty at the top level, and the
# first version of this file silently packaged no Qt at all and reported Qt6Core.dll as an
# unresolved dependency it expected Windows to provide.
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)

set(AIP_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package" CACHE PATH
    "Where `pixi run package` writes the portable folder.")

# Fills CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS with the redistributable CRT DLLs from the local Visual
# Studio. _SKIP stops the module installing them itself -- we only want the list.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON)
include(InstallRequiredSystemLibraries)

# Lists cannot survive a -D argument as themselves: the semicolons would be argument separators by
# the time the script sees them. Joined here, split there.
string(REPLACE ";" "|" AIP_PACKAGE_CRT_LIBS "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")

add_custom_target(aip_package
    COMMAND ${CMAKE_COMMAND}
        -D "AIP_PACKAGE_DIR=${AIP_PACKAGE_DIR}"
        -D "AIP_PACKAGE_EXECUTABLES=$<TARGET_FILE:aip_ui>|$<TARGET_FILE:aip_scan>"
        -D "AIP_PACKAGE_QT_PLUGIN_DIR=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
        -D "AIP_PACKAGE_SEARCH_DIRS=${QT6_INSTALL_PREFIX}/bin"
        -D "AIP_PACKAGE_CRT_LIBS=${AIP_PACKAGE_CRT_LIBS}"
        -P "${CMAKE_CURRENT_LIST_DIR}/package_impl.cmake"
    DEPENDS aip_ui aip_scan
    COMMENT "Building the portable package in ${AIP_PACKAGE_DIR}"
    VERBATIM
    USES_TERMINAL)
