# Catch2 test suite (design_doc.md sec. 6.1): protocol conformance (sec. 4.7), real-time safety
# (sec. 7.4.3) and source hygiene (sec. 6.6).
#
# The conformance harness is a first-class deliverable, not test scaffolding -- it replaces the
# predecessor's manual interactive DebugStream and is what lets the client be developed without
# audiodg.exe in the loop. It lives in a library of its own so the future APO stage can reuse it.

find_package(Catch2 3 REQUIRED)
include(Catch)

# The VST3 plugin the engine tests host. Sets AIP_TEST_PLUGIN_PATH in this scope.
add_subdirectory(fixtures/aip_test_plugin)

add_library(aip_harness STATIC
    harness/synthetic_king.cpp
)

target_include_directories(aip_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aip_harness PUBLIC aip_common aip_ipc aip_protocol aip_rt)

add_executable(aip_tests
    conformance_test.cpp
    engine_test.cpp
    planar_test.cpp
    protocol_layout_test.cpp
    realtime_safety_test.cpp
    source_hygiene_test.cpp
    spsc_queue_test.cpp
)

# The ASCII-only check (sec. 6.6) walks the source tree, so it needs to know where that is.
# CMake hands out forward slashes here, which is what we want inside a C++ string literal.
target_compile_definitions(aip_tests PRIVATE AIP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# The engine tests load a real VST3 bundle; the path is a generator expression because the
# output directory is per-configuration.
target_compile_definitions(aip_tests PRIVATE AIP_TEST_PLUGIN_PATH="${AIP_TEST_PLUGIN_PATH}")

# Nothing links the plugin, so nothing would otherwise build it before the tests run.
add_dependencies(aip_tests aip_test_plugin)

target_link_libraries(aip_tests PRIVATE
    aip_engine
    aip_harness
    Catch2::Catch2WithMain
    psapi              # GetProcessMemoryInfo, for the flat-RSS half of the sec. 7.4.3 criterion
    # The global allocation hooks must be linked into the executable itself for the real-time
    # violation detector to see anything (sec. 7.4.6).
    aip_rt_hooks
)

catch_discover_tests(aip_tests)
