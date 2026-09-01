# The two-effect module the one-to-many tests are pointed at (see the .cpp for why it exists).
#
# Same hand-built bundle layout as aip_test_plugin, and for the same reasons -- see that file's
# CMakeLists for why `smtg_add_vst3plugin` is not used.

add_library(aip_multi_plugin MODULE
    aip_multi_plugin.cpp
    # The DLL entry points, and the file that defines EditController::set/getEditorState. Both are
    # deliberately left out of the SDK's own targets (sec. 6.3.4, sec. 6.3.5).
    ${AIP_VST3_SDK_ROOT}/public.sdk/source/main/dllmain.cpp
    ${AIP_VST3_SDK_ROOT}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
)

set(AIP_MULTI_PLUGIN_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/aip_multi_plugin.vst3")

set_target_properties(aip_multi_plugin PROPERTIES
    PREFIX ""
    SUFFIX ".vst3"
    LIBRARY_OUTPUT_DIRECTORY "${AIP_MULTI_PLUGIN_BUNDLE_DIR}/Contents/x86_64-win")

target_link_libraries(aip_multi_plugin PRIVATE sdk)

if(MSVC)
    target_compile_options(aip_multi_plugin PRIVATE /external:W0)
endif()

set(AIP_MULTI_PLUGIN_PATH "${AIP_MULTI_PLUGIN_BUNDLE_DIR}" PARENT_SCOPE)
