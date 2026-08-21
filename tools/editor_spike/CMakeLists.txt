# editor_spike -- the sec. 5.1 de-risking spike. See main.cpp for what it is testing and why.
#
# This is the first target in the tree to link Qt, so it is also the first proof that the
# conda-forge qt6-main package, the Ninja Multi-Config generator and RelWithDebInfo-only building
# (sec. 6.4) actually work together.

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

# Deliberately not a WIN32 executable: the whole output of this tool is the console diagnostics,
# and a spike that cannot tell you why it failed is not worth running.
add_executable(editor_spike main.cpp)

set_target_properties(editor_spike PROPERTIES AUTOMOC ON)

target_link_libraries(editor_spike PRIVATE
    aip_engine
    Qt6::Widgets
    Qt6::Gui
)

# Qt's platform plugin lives outside the directory Qt6Core probes by default in a conda-forge
# layout, and without it the process dies with "could not find or load the Qt platform plugin".
# Handing the path to the executable next to it is more robust than relying on the environment.
add_custom_command(TARGET editor_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo
            "[Paths]" > "$<TARGET_FILE_DIR:editor_spike>/qt.conf"
    COMMAND ${CMAKE_COMMAND} -E echo
            "Plugins=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
            >> "$<TARGET_FILE_DIR:editor_spike>/qt.conf"
    VERBATIM)
