# ui -- the Qt 6 Widgets shell (design_doc.md sec. 5.1, sec. 7.1).
#
# An executable rather than a library, and its headers sit next to their sources rather than under
# `include/aip/ui/`: nothing links against this, so the public/private split the other components
# need would be a split with one side empty. `tools/editor_spike` set the precedent.

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

# WIN32 so no console window appears behind the shell. Everything a console would have carried is
# in the Counters group instead, which is the only place a user can see it anyway.
add_executable(aip_ui WIN32
    # The application icon, attached to the executable rather than to any window. Title bars are
    # stripped of theirs in src/window_chrome.cpp.
    aip_ui.rc
    src/main.cpp
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

set_target_properties(aip_ui PROPERTIES AUTOMOC ON)

# For the resource compiler, not for the C++ sources: rc.exe resolves a relative path in a .rc
# against its include path, not against the .rc file's own directory, so without this the icon is
# reported as missing.
target_include_directories(aip_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(aip_ui PRIVATE
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

# aip_ui starts aip_scan; nothing links it, so nothing would otherwise build it first.
add_dependencies(aip_ui aip_scan)

# Qt's platform plugin lives outside the directory Qt6Core probes by default in a conda-forge
# layout, and without it the process dies with "could not find or load the Qt platform plugin" --
# and, being a WIN32 executable, dies with nowhere to say so. Writing the path next to the
# executable is more robust than relying on the environment.
add_custom_command(TARGET aip_ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
            "[Paths]" > "$<TARGET_FILE_DIR:aip_ui>/qt.conf"
    COMMAND ${CMAKE_COMMAND} -E echo
            "Plugins=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
            >> "$<TARGET_FILE_DIR:aip_ui>/qt.conf"
    VERBATIM)
