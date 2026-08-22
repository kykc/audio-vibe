# Run by the `aip_package` target through `cmake -P`; see package.cmake for what and why.
#
# Script mode, so none of the project's variables exist here -- everything arrives through -D.

cmake_minimum_required(VERSION 3.28)

# CMake 4 normalizes the paths it matches the exclude patterns against; without this it warns,
# once per dependency, about the difference between `C:\WINDOWS\system32/x.dll` and the forward
# slash form. Guarded because the policy does not exist at our version floor.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

foreach(required AIP_PACKAGE_DIR AIP_PACKAGE_EXECUTABLES AIP_PACKAGE_QT_PLUGIN_DIR
                 AIP_PACKAGE_SEARCH_DIRS)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to package_impl.cmake")
    endif()
endforeach()

string(REPLACE "|" ";" AIP_PACKAGE_EXECUTABLES "${AIP_PACKAGE_EXECUTABLES}")
string(REPLACE "|" ";" AIP_PACKAGE_CRT_LIBS "${AIP_PACKAGE_CRT_LIBS}")
string(REPLACE "|" ";" AIP_PACKAGE_SEARCH_DIRS "${AIP_PACKAGE_SEARCH_DIRS}")

# Which Qt plugins go in. A dependency walk cannot discover these -- they are loaded by name at
# run time -- so the list is a decision rather than a discovery:
#
#   platforms      qwindows.dll is not optional. Without it the process exits with "could not
#                  find or load the Qt platform plugin", and being a WIN32 executable it exits
#                  with nowhere to say so (status.md sec. 8 item 21).
#   styles         the native Windows 11 look. Without it the shell renders in Qt's Fusion style,
#                  which works and looks foreign.
#   imageformats   nothing loads an image today -- the application icon is a Win32 resource in
#                  the executable, not something Qt reads. Cheap, and the alternative is finding
#                  out on someone else's machine the first time an icon is added.
#   iconengines    same reasoning, for the SVG icon engine.
set(AIP_QT_PLUGIN_DIRS platforms styles imageformats iconengines)

# ------------------------------------------------------------------------------------- the folder

message(STATUS "Packaging into ${AIP_PACKAGE_DIR}")
file(REMOVE_RECURSE "${AIP_PACKAGE_DIR}")
file(MAKE_DIRECTORY "${AIP_PACKAGE_DIR}")

set(staged_executables)
foreach(executable IN LISTS AIP_PACKAGE_EXECUTABLES)
    if(NOT EXISTS "${executable}")
        message(FATAL_ERROR "${executable} does not exist -- build before packaging")
    endif()
    file(COPY "${executable}" DESTINATION "${AIP_PACKAGE_DIR}")
    get_filename_component(name "${executable}" NAME)
    list(APPEND staged_executables "${AIP_PACKAGE_DIR}/${name}")
endforeach()

# `aip_scan.exe` next to `aip_ui.exe` is not tidiness: the scanner looks for its child beside the
# running executable first, and falls back to a compile-time path that names this build tree
# (status.md sec. 7 item 42). A package without it reports every plugin on the machine as broken.

set(staged_plugins)
foreach(plugin_dir IN LISTS AIP_QT_PLUGIN_DIRS)
    set(source "${AIP_PACKAGE_QT_PLUGIN_DIR}/${plugin_dir}")
    if(NOT IS_DIRECTORY "${source}")
        message(WARNING "Qt plugin directory ${source} does not exist; skipping")
        continue()
    endif()
    file(GLOB dlls "${source}/*.dll")
    foreach(dll IN LISTS dlls)
        file(COPY "${dll}" DESTINATION "${AIP_PACKAGE_DIR}/plugins/${plugin_dir}")
        get_filename_component(name "${dll}" NAME)
        list(APPEND staged_plugins "${AIP_PACKAGE_DIR}/plugins/${plugin_dir}/${name}")
    endforeach()
endforeach()

# ------------------------------------------------------------------------------- the dependencies

# The plugins go in as MODULES, not EXECUTABLES: they are not linked into anything, and their own
# imports are part of what has to be shipped.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES ${staged_executables}
    MODULES ${staged_plugins}
    DIRECTORIES ${AIP_PACKAGE_SEARCH_DIRS}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    # Everything Windows itself provides. `api-ms-win-*` and `ext-ms-win-*` are the API sets that
    # front the system CRT and are never redistributed; the rest is the usual system surface.
    PRE_EXCLUDE_REGEXES
        "^api-ms-.*"
        "^ext-ms-.*"
        "^(kernel32|user32|gdi32|shell32|ole32|oleaut32|advapi32|comdlg32|comctl32)\\.dll$"
        "^(ws2_32|winmm|avrt|mmdevapi|version|imm32|dwmapi|uxtheme|d3d11|dxgi|d3d9|opengl32)\\.dll$"
        "^(setupapi|winspool|netapi32|userenv|crypt32|bcrypt|ncrypt|secur32|shlwapi)\\.dll$"
        "^(msvcrt|ntdll|rpcrt4|sechost|combase|psapi|powrprof|wtsapi32|dbghelp)\\.dll$"
    # And anything that resolved into Windows anyway. The CRT is handled separately below, from
    # the redistributable copy rather than from System32.
    POST_EXCLUDE_REGEXES
        "[Ss]ystem32"
        "[Ww]inSxS"
)

foreach(dependency IN LISTS resolved)
    file(COPY "${dependency}" DESTINATION "${AIP_PACKAGE_DIR}")
endforeach()

# Unresolved is not automatically a failure -- a name Windows provides that the excludes above did
# not name still lands here -- but it is the one place a missing DLL shows up before someone else's
# machine does the reporting, so it is printed rather than swallowed.
if(unresolved)
    message(STATUS "Not resolved (expected to be provided by Windows):")
    foreach(name IN LISTS unresolved)
        message(STATUS "  ${name}")
    endforeach()
endif()

foreach(runtime IN LISTS AIP_PACKAGE_CRT_LIBS)
    if(EXISTS "${runtime}")
        file(COPY "${runtime}" DESTINATION "${AIP_PACKAGE_DIR}")
    endif()
endforeach()

# ------------------------------------------------------------------------------------ the settings

# Pins Qt to this folder. Without it Qt falls back to the prefix compiled into Qt6Core, which is
# the path the *build* machine's environment happened to live at -- so a package tested here would
# quietly load the developer's plugins and pass, and the same package would fail on the machine it
# was made for. Relative paths in qt.conf are relative to the executable.
file(WRITE "${AIP_PACKAGE_DIR}/qt.conf"
"[Paths]\n\
Prefix = .\n\
Plugins = plugins\n")

# An empty session file next to the executable is how portable mode is asked for
# (config/session_file.h). Shipping one is the point of this folder: settings stay in it rather
# than in the AppData of whichever machine it is run on.
file(WRITE "${AIP_PACKAGE_DIR}/aip_config.yaml"
"# audio-ipc2 portable session.\n\
#\n\
# This file being here, next to aip_ui.exe, is what makes this a portable install: the rack, the\n\
# plugin scan and the window position are kept in it rather than in %APPDATA%. Delete it to use\n\
# %APPDATA% instead. It is rewritten in full every time the shell closes.\n\
version: 1\n")

file(GLOB packaged "${AIP_PACKAGE_DIR}/*.dll")
list(LENGTH packaged dll_count)
message(STATUS "Packaged ${dll_count} DLL(s) into ${AIP_PACKAGE_DIR}")
