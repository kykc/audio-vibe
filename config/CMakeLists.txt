# The session file (status.md sec. 5): what the shell remembers between runs, and where it keeps
# it -- and the chain preset, which is the same rack in a file the user names. Depends on the
# engine because a saved rack is a rack of real plugins; deliberately does not depend on Qt, so
# the whole of it is testable without a window.

add_library(aip_config STATIC
    src/attach_guard.cpp
    src/base64.cpp
    src/file_stamp.cpp
    src/load_guard.cpp
    src/preset_file.cpp
    src/rack_yaml.h
    src/session.cpp
    src/session_file.cpp
)

target_include_directories(aip_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(aip_config
    PUBLIC
        aip_common
        aip_engine
        # For ScannedModule only -- the cached scan report is part of what is saved. This is the
        # parent half of scanner/, which loads nothing; the child is a separate executable.
        aip_scanner
    PRIVATE
        yaml-cpp::yaml-cpp
        # SHGetKnownFolderPath and CoTaskMemFree, for the AppData half of sec. 5's config
        # locations. Both are in MSVC's default library list already; named here so the
        # dependency is visible rather than inherited.
        shell32
        ole32
)

target_sources(aip_config PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/aip/config/attach_guard.h
        include/aip/config/base64.h
        include/aip/config/file_stamp.h
        include/aip/config/load_guard.h
        include/aip/config/preset_file.h
        include/aip/config/session.h
        include/aip/config/session_file.h
)
