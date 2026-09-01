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

# The bundle shaped like LSP's -- a mono effect first, a stereo one after it. Sets
# AIP_MULTI_PLUGIN_PATH.
add_subdirectory(fixtures/aip_multi_plugin)

# The two that misbehave on purpose, for the scanner. Sets AIP_CRASH_PLUGIN_PATH and
# AIP_HANG_PLUGIN_PATH.
add_subdirectory(fixtures/aip_hostile_plugin)

add_library(aip_harness STATIC
    harness/synthetic_king.cpp
    harness/valet_driver.cpp
)

target_include_directories(aip_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aip_harness PUBLIC aip_common aip_ipc aip_protocol aip_rt)

# The apo/ king is compiled into the suite from source rather than linked: `aip_apo` is a /MT
# DLL and this is a /MD executable (apo/CMakeLists.txt). Same source, same behaviour, no second
# copy of the code -- and it means the king is covered by ctest without a DLL, audiodg.exe or an
# elevated runner anywhere in the picture.
add_executable(aip_tests
    ${CMAKE_SOURCE_DIR}/apo/src/buffer_king.cpp
    apo_dll_test.cpp
    apo_king_test.cpp
    apo_registration_test.cpp
    config_test.cpp
    conformance_test.cpp
    engine_test.cpp
    output_meter_test.cpp
    planar_test.cpp
    protocol_layout_test.cpp
    realtime_safety_test.cpp
    scanner_test.cpp
    signal_source_test.cpp
    source_hygiene_test.cpp
    spsc_queue_test.cpp
)

target_include_directories(aip_tests PRIVATE ${CMAKE_SOURCE_DIR}/apo/include)

# The APO DLL is loaded at run time by apo_dll_test.cpp -- /MT DLL, /MD suite, and loading it is
# what the audio engine does too. Nothing links it, so nothing would otherwise build it first.
add_dependencies(aip_tests aip_apo)
target_compile_definitions(aip_tests PRIVATE
    AIP_APO_DLL_PATH="$<TARGET_FILE:aip_apo>"
    AIP_APO_CLSID=L"{C6A6A861-A99F-4F00-B636-657F38F353E9}")

# The ASCII-only check (sec. 6.6) walks the source tree, so it needs to know where that is.
# CMake hands out forward slashes here, which is what we want inside a C++ string literal.
target_compile_definitions(aip_tests PRIVATE AIP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# The engine tests load a real VST3 bundle; the path is a generator expression because the
# output directory is per-configuration.
target_compile_definitions(aip_tests PRIVATE AIP_TEST_PLUGIN_PATH="${AIP_TEST_PLUGIN_PATH}")
target_compile_definitions(aip_tests PRIVATE AIP_MULTI_PLUGIN_PATH="${AIP_MULTI_PLUGIN_PATH}")

target_compile_definitions(aip_tests PRIVATE
    AIP_CRASH_PLUGIN_PATH="${AIP_CRASH_PLUGIN_PATH}"
    AIP_HANG_PLUGIN_PATH="${AIP_HANG_PLUGIN_PATH}")

# Nothing links any of these, so nothing would otherwise build them before the tests run. aip_scan
# is the one that would fail most confusingly: the scanner tests would report every plugin as
# broken, which is precisely what a scanner reports when it cannot find its own child.
add_dependencies(aip_tests aip_test_plugin aip_multi_plugin aip_crash_plugin aip_hang_plugin aip_scan)

target_link_libraries(aip_tests PRIVATE
    aip_config
    aip_engine
    aip_harness
    aip_scanner
    aip_signal         # the apo_host signal bank, whose validation rules have their own cases
    Catch2::Catch2WithMain
    audiobaseprocessingobject  # APOERR_ALREADY_UNLOCKED and the APO interface GUIDs
    psapi              # GetProcessMemoryInfo, for the flat-RSS half of the sec. 7.4.3 criterion
    # The global allocation hooks must be linked into the executable itself for the real-time
    # violation detector to see anything (sec. 7.4.6).
    aip_rt_hooks
)

# DISCOVERY_MODE PRE_TEST is not a preference -- POST_BUILD is actively wrong in a multi-config
# build. It writes *one* test list, naming the executable of whichever configuration was built
# last, and `ctest -C <other>` then happily runs that one instead. Building Release and then
# running the RelWithDebInfo suite reports "100% tests passed out of 44" while the five tests that
# need AIP_RT_CHECKS skip themselves, because they are correctly compiled out of the binary being
# run -- and skipped tests do not fail a ctest run. The single most important assertion in this
# project silently stops being made, which is trap 4 all over again with a different cause.
#
# PRE_TEST defers discovery to ctest time, where $<CONFIG> is known, so each configuration gets
# its own list of its own binary. See status.md sec. 8 item 19.
catch_discover_tests(aip_tests DISCOVERY_MODE PRE_TEST)
