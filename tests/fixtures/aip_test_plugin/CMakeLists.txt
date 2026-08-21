# The VST3 plugin the engine tests host (see aip_test_plugin.cpp for why it exists).
#
# Built by hand rather than through `smtg_add_vst3plugin`. That macro lives in the SDK's module
# directory, which the SDK adds to CMAKE_MODULE_PATH in its own scope only, and it depends on a
# spread of SMTG_* variables that `smtg_setup_platform_toolset()` sets in that same scope.
# Reaching in for it would couple us to the SDK's CMake internals -- the part of the SDK sec. 6.3.5
# already says not to depend on. A VST3 module on Windows is a DLL named `.vst3` sitting in a
# fixed directory layout, and that is all the code below arranges.

add_library(aip_test_plugin MODULE
    aip_test_plugin.cpp
    # The DLL entry points. `sdk` provides the factory machinery but not dllmain, the same
    # division of labour as sdk_hosting and module_win32.cpp (sec. 6.3.4).
    ${AIP_VST3_SDK_ROOT}/public.sdk/source/main/dllmain.cpp
    # Nor is SingleComponentEffect part of `sdk`, and it is the file that happens to define
    # EditController::set/getEditorState -- so leaving it out produces fifteen LNK2001s, most of
    # them naming a class the plugin never mentions.
    ${AIP_VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
)

# The bundle layout Module::create expects when handed a directory:
#   <name>.vst3/Contents/x86_64-win/<name>.vst3
# Building straight into it beats copying afterwards -- the DLL and the bundle would otherwise
# want the same path. Handing Module::create a bare DLL also works, but that is the legacy shape
# and not what the tests should be exercising.
set(AIP_TEST_PLUGIN_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/aip_test_plugin.vst3")

set_target_properties(aip_test_plugin PROPERTIES
    PREFIX ""
    SUFFIX ".vst3"
    LIBRARY_OUTPUT_DIRECTORY "${AIP_TEST_PLUGIN_BUNDLE_DIR}/Contents/x86_64-win")

# GetPluginFactory / InitDll / ExitDll are exported by __declspec(dllexport) from the SDK's own
# macros, so no module definition file is needed.
target_link_libraries(aip_test_plugin PRIVATE sdk)

if(MSVC)
    target_compile_options(aip_test_plugin PRIVATE /external:W0)
endif()

# Handed to the test executable as a compile definition. A test has no reliable way to work this
# out at run time, and a guessed relative path would be the first thing to rot.
set(AIP_TEST_PLUGIN_PATH "${AIP_TEST_PLUGIN_BUNDLE_DIR}" PARENT_SCOPE)
