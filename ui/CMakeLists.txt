# ui -- the Qt 6 Widgets shell (design_doc.md sec. 5.1, sec. 7.1).
#
# An executable rather than a library, and its headers sit next to their sources rather than under
# `include/aip/ui/`: nothing links against this, so the public/private split the other components
# need would be a split with one side empty. `tools/editor_spike` set the precedent.

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

# WIN32 so no console window appears behind the shell. Everything a console would have carried is
# in the Counters group instead, which is the only place a user can see it anyway.
add_executable(vibeaudio WIN32
    # The application icon, declared here and nowhere else. Every window wears it
    # (design_doc.md sec. 5.6); src/window_chrome.cpp reads it back out of the running
    # executable so that there is no second copy of the picture to keep in step.
    vibeaudio.rc
    src/main.cpp
    src/about_dialog.cpp
    src/about_dialog.h
    src/apo_admin_runner.cpp
    src/apo_admin_runner.h
    src/audio_device_dialog.cpp
    src/audio_device_dialog.h
    # The first Qt Designer form in the project. AUTOUIC turns it into ui_audio_device_dialog.h
    # in the build tree; nothing includes it but src/audio_device_dialog.cpp.
    src/audio_device_dialog.ui
    src/main_window.cpp
    src/main_window.h
    src/session_end_filter.cpp
    src/session_end_filter.h
    src/rack_list.cpp
    src/rack_list.h
    src/rack_panel.cpp
    src/rack_panel.h
    src/plugin_catalog.cpp
    src/plugin_catalog.h
    src/plugin_picker.cpp
    src/plugin_picker.h
    src/engine_host.cpp
    src/engine_host.h
    src/editor_manager.cpp
    src/editor_manager.h
    src/editor_window.cpp
    src/editor_window.h
    src/generic_editor_window.cpp
    src/generic_editor_window.h
    src/plugin_editor_window.h
    src/plug_frame.cpp
    src/plug_frame.h
    src/process_footprint.cpp
    src/process_footprint.h
    src/window_chrome.cpp
    src/window_chrome.h
    src/window_placement.cpp
    src/window_placement.h
)

# AUTOUIC as well as AUTOMOC now: `src/audio_device_dialog.ui` is the first form laid out in
# Designer rather than in C++ (see the header for why that one and not the others). uic puts
# `ui_audio_device_dialog.h` in the build tree and on the include path, so there is nothing else
# to declare -- and nothing generated ever lands in the source tree.
set_target_properties(vibeaudio PROPERTIES AUTOMOC ON AUTOUIC ON)

# For the resource compiler, not for the C++ sources: rc.exe resolves a relative path in a .rc
# against its include path, not against the .rc file's own directory, so without this the icon is
# reported as missing.
target_include_directories(vibeaudio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# `aip/version.h`, which the About box reads. Written on every build rather than at configure time
# so that the commit it names is the commit the binary came from -- cmake/version.cmake says why
# that distinction is worth a target.
target_include_directories(vibeaudio PRIVATE ${AIP_VERSION_INCLUDE_DIR})
add_dependencies(vibeaudio aip_version_header)

# The same version, in the file's own metadata rather than only in a dialog the user has to open
# (cmake/version.cmake). This adds a second .rc to the target beside vibeaudio.rc; two resource
# scripts on one binary are fine as long as nothing in them collides, and an icon and a
# VERSIONINFO are different resource types entirely.
aip_add_version_resource(vibeaudio DESCRIPTION "VibeAudio")

target_link_libraries(vibeaudio PRIVATE
    aip_common
    # The session file: what the shell remembers between runs (status.md sec. 5).
    aip_config
    aip_engine
    aip_ipc
    # The parent half only: ui/ asks for a scan, aip_scan does the loading (sec. 7.2).
    aip_scanner
    aip_rt_hooks
    Qt6::Widgets
    Qt6::Gui
    # GetProcessMemoryInfo, for the resident-set half of the sec. 7.4.3 criterion on screen.
    psapi
)

# vibeaudio starts aip_scan; nothing links it, so nothing would otherwise build it first.
add_dependencies(vibeaudio aip_scan)

# The same arrangement for apo_admin, which the File menu runs elevated to do the two things the
# shell has no rights to do itself -- register the APO, and put it into an endpoint's effect chain.
# In the package both sit in one flat folder and "next to me" finds it; in the build tree every
# target has its own directory, so the path is compiled in as the fallback -- exactly what
# scanner/CMakeLists.txt does for aip_scan.
add_dependencies(vibeaudio apo_admin)
target_compile_definitions(vibeaudio PRIVATE AIP_APO_ADMIN_EXECUTABLE="$<TARGET_FILE:apo_admin>")

# And once more for the APO itself, which Register APO hands to apo_admin as the file to copy.
# Nothing links it -- the shell never loads the APO, the audio engine does -- so this is purely
# "know where it is, and build it first".
add_dependencies(vibeaudio aip_apo)
target_compile_definitions(vibeaudio PRIVATE AIP_APO_DLL="$<TARGET_FILE:aip_apo>")

# Qt's platform plugin lives outside the directory Qt6Core probes by default in a conda-forge
# layout, and without it the process dies with "could not find or load the Qt platform plugin" --
# and, being a WIN32 executable, dies with nowhere to say so. Writing the path next to the
# executable is more robust than relying on the environment.
add_custom_command(TARGET vibeaudio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
            "[Paths]" > "$<TARGET_FILE_DIR:vibeaudio>/qt.conf"
    COMMAND ${CMAKE_COMMAND} -E echo
            "Plugins=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
            >> "$<TARGET_FILE_DIR:vibeaudio>/qt.conf"
    VERBATIM)
