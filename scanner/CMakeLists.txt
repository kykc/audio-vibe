# Out-of-process plugin probing (design_doc.md sec. 7.2).
#
# Two halves in one directory, and the split matters more than the sharing:
#
#   aip_scanner  a static library with both sides of the conversation in it. The parent side
#                (scanner.cpp) loads nothing and is what the shell links. The child side
#                (probe_worker.cpp) loads plugins and must only ever run inside aip_scan.
#   aip_scan     the executable that is allowed to die. Thin: argument parsing, a handle, a loop.
#
# They share a directory because they share a wire format, and a codec whose two ends can drift
# apart is a codec that will.

add_library(aip_scanner STATIC
    src/probe_worker.cpp
    src/scan_record.cpp
    src/scanner.cpp
)

target_include_directories(aip_scanner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PRIVATE, and that is the point: nothing in aip_scanner's headers mentions the VST3 SDK, so the
# shell can ask for a scan without becoming a plugin host. CMake still propagates the dependency
# for linking, which is all a consumer of a static library needs.
target_link_libraries(aip_scanner
    PUBLIC
        aip_common
    PRIVATE
        aip_engine
        aip_ipc
)

target_sources(aip_scanner PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/aip/scanner/probe_worker.h
        include/aip/scanner/scan_record.h
        include/aip/scanner/scan_result.h
        include/aip/scanner/scanner.h
)

add_executable(aip_scan src/scan_main.cpp)

target_link_libraries(aip_scan PRIVATE
    aip_engine
    aip_ipc
    aip_scanner
)

# How the parent finds the child in a build tree. Ninja Multi-Config gives every target its own
# output directory, so `aip_scan.exe` is not next to `vibeaudio.exe` here even though an installed
# layout will put them side by side -- and "next to the running executable" has to stay the first
# answer, because that is the one that will be true in the field. This is the same escape hatch
# tests/ uses for AIP_TEST_PLUGIN_PATH: a compile definition beats a relative path that rots.
target_compile_definitions(aip_scanner PRIVATE AIP_SCAN_EXECUTABLE="$<TARGET_FILE:aip_scan>")

# What Explorer's Details tab and Task Manager show about this file (cmake/version.cmake). The
# version in it is the one the About box quotes, resolved on the same build.
aip_add_version_resource(aip_scan DESCRIPTION "VibeAudio plugin scanner")
