# The portable package: one folder that runs `vibeaudio.exe` on a machine that has none of this
# installed -- no pixi environment, no Qt, no Visual Studio -- and also registers and manages the
# APO on it.
#
# The APO half used to live in an `apo/` subfolder, on the argument that a folder boundary is worth
# having between running the shell and rewriting machine state. It is flat now, which costs that
# signpost and buys two things. The first is that `regsvr32 aip_apo.dll` and `vibeaudio.exe` are run
# from the same directory, so the instructions have no "from the folder above" in them. The second
# is that there is only one directory whose imports have to resolve, which is the thing Windows
# actually cares about -- see `aip_stage_folder`.
#
# What the boundary was saying still has to be said, so `README.txt` says it in words: everything
# except `apo_admin --list` needs elevation, and an installed APO outlives the folder it came from.
#
# Two kinds of thing have to be in it, and only the first is found by looking at the executable:
#
#   imported DLLs    Qt6Core/Gui/Widgets and everything *they* import, which on a conda-forge Qt
#                    is a long tail of separate packages -- harfbuzz, freetype, pcre2, zlib, icu,
#                    double-conversion, md4c and so on. `file(GET_RUNTIME_DEPENDENCIES)` walks the
#                    import tables transitively and resolves each name against the environment.
#   Qt plugins       loaded by name at run time, so nothing imports them and no dependency walk
#                    can find them. Listed explicitly below, and then fed back into the walk,
#                    because a plugin has imports of its own.
#
# What is deliberately **not** in it is the MSVC runtime -- `msvcp140*.dll`, `vcruntime140*.dll`,
# `concrt140.dll`. It is a machine prerequisite instead (design_doc.md sec. 6.7): the user installs
# the Microsoft Visual C++ 2015-2022 x64 redistributable, which puts a serviced copy in System32
# where every application on the machine shares it and Windows Update patches it. Shipping our own
# copy in the package folder would shadow that for our two processes and freeze them on whatever
# version this build tree happened to hold, security fixes included. The excludes in
# `package_impl.cmake` keep the dependency walk from quietly putting them back, because
# conda-forge's Qt carries its own copies in the very directory the walk searches.
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

# There is deliberately no `InstallRequiredSystemLibraries` here. It is the module that names the
# local Visual Studio's redistributable CRT DLLs so they can be copied into a package, and this
# package does not copy them -- see the header comment. Its absence is the decision, so it is
# written down rather than left as a thing nobody thought of.
#
# Lists cannot survive a -D argument as themselves: the semicolons would be argument separators by
# the time the script sees them, which is why the multi-value ones below are joined with `|` here
# and split there.

add_custom_target(aip_package
    COMMAND ${CMAKE_COMMAND}
        -D "AIP_PACKAGE_DIR=${AIP_PACKAGE_DIR}"
        # The shell, its scanner, and the two APO tools -- all four ordinary executables, all four
        # in the same directory now, so the dependency walk sees them as one set.
        -D "AIP_PACKAGE_EXECUTABLES=$<TARGET_FILE:vibeaudio>|$<TARGET_FILE:aip_scan>|$<TARGET_FILE:apo_admin>|$<TARGET_FILE:apo_host>"
        # `aip_apo.dll` is a MODULE to the dependency walk and not an executable -- nothing links
        # it, and `audiodg.exe` loads it by the path `regsvr32` recorded.
        -D "AIP_PACKAGE_MODULES=$<TARGET_FILE:aip_apo>"
        -D "AIP_PACKAGE_QT_PLUGIN_DIR=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
        -D "AIP_PACKAGE_SEARCH_DIRS=${QT6_INSTALL_PREFIX}/bin"
        -P "${CMAKE_CURRENT_LIST_DIR}/package_impl.cmake"
    DEPENDS vibeaudio aip_scan aip_apo apo_admin apo_host
    COMMENT "Building the portable package in ${AIP_PACKAGE_DIR}, and ${AIP_PACKAGE_DIR}.zip beside it"
    VERBATIM
    USES_TERMINAL)
