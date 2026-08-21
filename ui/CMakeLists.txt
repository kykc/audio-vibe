# ui -- the Qt 6 Widgets shell (design_doc.md sec. 5.1, sec. 7.1).
#
# An executable rather than a library, and its headers sit next to their sources rather than under
# `include/aip/ui/`: nothing links against this, so the public/private split the other components
# need would be a split with one side empty. `tools/editor_spike` set the precedent.

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

# WIN32 so no console window appears behind the shell. Everything a console would have carried is
# in the Counters group instead, which is the only place a user can see it anyway.
add_executable(aip_ui WIN32
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/rack_panel.cpp
    src/rack_panel.h
    src/plugin_picker.cpp
    src/plugin_picker.h
    src/engine_host.cpp
    src/engine_host.h
    src/editor_manager.cpp
    src/editor_manager.h
    src/editor_window.cpp
    src/editor_window.h
    src/plug_frame.cpp
    src/plug_frame.h
)

set_target_properties(aip_ui PROPERTIES AUTOMOC ON)

target_link_libraries(aip_ui PRIVATE
    aip_common
    aip_engine
    aip_ipc
    aip_rt_hooks
    Qt6::Widgets
    Qt6::Gui
)

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
