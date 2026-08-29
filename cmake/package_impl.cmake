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
                 AIP_PACKAGE_SEARCH_DIRS AIP_PACKAGE_MODULES)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} was not passed to package_impl.cmake")
    endif()
endforeach()

string(REPLACE "|" ";" AIP_PACKAGE_EXECUTABLES "${AIP_PACKAGE_EXECUTABLES}")
string(REPLACE "|" ";" AIP_PACKAGE_MODULES "${AIP_PACKAGE_MODULES}")
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

# ------------------------------------------------------------------------------ staging a folder

# Everything Windows itself provides. `api-ms-win-*` and `ext-ms-win-*` are the API sets that
# front the system CRT and are never redistributed; the rest is the usual system surface.
#
# The MSVC runtime is in this list for a different reason than the rest, and the reason matters
# enough to say twice: it is not shipped because it is a machine prerequisite (design_doc.md
# sec. 6.7), not because Windows provides it. Excluding it by name is not optional -- conda-forge's
# Qt ships its own `msvcp140.dll` and friends in `Library/bin`, which is exactly the directory the
# walk searches, so without these two patterns the walk resolves them there and copies them in
# whatever this file says it does not do.
set(AIP_SYSTEM_DLL_REGEXES
    "^api-ms-.*"
    "^ext-ms-.*"
    "^(msvcp140|vcruntime140|concrt140|vccorlib140).*\.dll$"
    "^(kernel32|user32|gdi32|shell32|ole32|oleaut32|advapi32|comdlg32|comctl32)\.dll$"
    "^(ws2_32|winmm|avrt|mmdevapi|version|imm32|dwmapi|uxtheme|d3d11|dxgi|d3d9|opengl32)\.dll$"
    "^(setupapi|winspool|netapi32|userenv|crypt32|bcrypt|ncrypt|secur32|shlwapi)\.dll$"
    "^(msvcrt|ntdll|rpcrt4|sechost|combase|psapi|powrprof|wtsapi32|dbghelp)\.dll$"
)

# And anything that resolved into Windows anyway -- which is also where the machine's own MSVC
# runtime lives, so this catches it a second time on a machine that has it installed.
set(AIP_SYSTEM_PATH_REGEXES "[Ss]ystem32" "[Ww]inSxS")

# Walks the import tables of what has already been copied into `destination` and copies what they
# need in beside them.
#
# Takes the folder as an argument, and is still worded that way, because the reason it used to be
# called twice has not gone away: Windows resolves an executable's imports against its own
# directory and never against the parent's, so a DLL that `apo_admin.exe` needs has to sit beside
# it. That constraint is what a flat package satisfies for free -- there is one directory, so
# anything the walk finds is beside everything that asked for it -- and it is the reason the
# `apo/` subfolder needed its own walk rather than inheriting the shell's.
function(aip_stage_folder destination)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "EXECUTABLES;MODULES")

    # The Qt plugins and `aip_apo.dll` go in as MODULES, not EXECUTABLES: they are not linked into
    # anything, and their own imports are part of what has to be shipped.
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES ${ARG_EXECUTABLES}
        MODULES ${ARG_MODULES}
        DIRECTORIES ${AIP_PACKAGE_SEARCH_DIRS}
        RESOLVED_DEPENDENCIES_VAR resolved
        UNRESOLVED_DEPENDENCIES_VAR unresolved
        PRE_EXCLUDE_REGEXES ${AIP_SYSTEM_DLL_REGEXES}
        POST_EXCLUDE_REGEXES ${AIP_SYSTEM_PATH_REGEXES}
    )

    foreach(dependency IN LISTS resolved)
        file(COPY "${dependency}" DESTINATION "${destination}")
    endforeach()

    # Unresolved is not automatically a failure -- everything excluded above lands here, which now
    # includes the MSVC runtime the target machine is expected to have -- but it is the one place a
    # genuinely missing DLL shows up before someone else's machine does the reporting, so it is
    # printed rather than swallowed.
    if(unresolved)
        message(STATUS "Not resolved for ${destination} "
                       "(expected from Windows or the VC++ redistributable):")
        foreach(name IN LISTS unresolved)
            message(STATUS "  ${name}")
        endforeach()
    endif()
endfunction()

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

# `aip_scan.exe` next to `vibeaudio.exe` is not tidiness: the scanner looks for its child beside the
# running executable first, and falls back to a compile-time path that names this build tree
# (status.md sec. 7 item 42). A package without it reports every plugin on the machine as broken.

# `aip_apo.dll` beside them is not tidiness either, and for a heavier reason: its path is
# *recorded*. `regsvr32` writes this location into `InprocServer32` and the audio engine loads the
# DLL from there ever after, so the package folder is the deployment location and not a staging
# area. `apo_host.exe` also wants the DLL beside it, which the same copy satisfies -- its default
# `--dll` is "next to this executable".
#
# Flat rather than in `apo/` means that recorded path is now the folder holding Qt as well, so the
# whole package has to stay where it was registered and stay readable by the audio service account.
# `README.txt` says both.
set(staged_modules)
foreach(module IN LISTS AIP_PACKAGE_MODULES)
    if(NOT EXISTS "${module}")
        message(FATAL_ERROR "${module} does not exist -- build before packaging")
    endif()
    file(COPY "${module}" DESTINATION "${AIP_PACKAGE_DIR}")
    get_filename_component(name "${module}" NAME)
    list(APPEND staged_modules "${AIP_PACKAGE_DIR}/${name}")
endforeach()

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

# One walk, over everything, because there is one directory. `aip_apo.dll` contributes nothing to
# it in practice and is in it for the day it does: it is /MT and imports only AVRT, ole32, ADVAPI32
# and KERNEL32 -- deliberately, since a VC redistributable inside `audiodg.exe` is exactly what the
# static CRT avoids (apo/CMakeLists.txt). The APO tools are ordinary /MD executables, so what they
# need is the MSVC runtime, which the machine provides and this package does not carry.
aip_stage_folder("${AIP_PACKAGE_DIR}"
    EXECUTABLES ${staged_executables}
    MODULES ${staged_plugins} ${staged_modules})

# The operating instructions, in the folder they operate on. A file of its own rather than a string
# here: it is forty lines of prose containing Windows paths, and every backslash in it would need
# doubling to survive CMake's parser. `apo/README.md` is a different document -- that one is about
# the source tree and the design; this one is the commands, in order, and the two ways the whole
# thing silently does nothing.
configure_file("${CMAKE_CURRENT_LIST_DIR}/apo_readme.txt" "${AIP_PACKAGE_DIR}/README.txt" COPYONLY)

# The license, beside the software it covers. MIT asks for the notice to travel with the software,
# and a portable folder somebody was handed on a stick is exactly the case that has nothing else
# to read. (Qt ships here under the LGPLv3 and its own license text is not in the folder yet.)
configure_file("${CMAKE_CURRENT_LIST_DIR}/../LICENSE" "${AIP_PACKAGE_DIR}/LICENSE" COPYONLY)

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
file(WRITE "${AIP_PACKAGE_DIR}/vibeaudio.yaml"
"# VibeAudio portable session.\n\
#\n\
# This file being here, next to vibeaudio.exe, is what makes this a portable install: the rack, the\n\
# plugin scan and the window position are kept in it rather than in %APPDATA%. Delete it to use\n\
# %APPDATA% instead. It is rewritten in full every time the shell closes.\n\
version: 1\n")

file(GLOB packaged_dlls "${AIP_PACKAGE_DIR}/*.dll")
list(LENGTH packaged_dlls dll_count)
file(GLOB packaged_exes "${AIP_PACKAGE_DIR}/*.exe")
list(LENGTH packaged_exes exe_count)
message(STATUS "Packaged ${exe_count} executable(s) and ${dll_count} DLL(s)"
               " into ${AIP_PACKAGE_DIR}")
