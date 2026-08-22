# The two deliberately broken plugins the scanner tests are pointed at (see the .cpp for why).
#
# Same hand-built bundle layout as aip_test_plugin, and for the same reasons -- see that file's
# CMakeLists for why `smtg_add_vst3plugin` is not used. These are smaller still: they link nothing
# at all, because a DLL that faults inside GetPluginFactory never needs the SDK to reach it.

set(AIP_HOSTILE_VARIANTS
    aip_crash_plugin AIP_HOSTILE_CRASH
    aip_hang_plugin  AIP_HOSTILE_HANG
)

list(LENGTH AIP_HOSTILE_VARIANTS aip_hostile_count)
math(EXPR aip_hostile_last "${aip_hostile_count} - 1")

foreach(index RANGE 0 ${aip_hostile_last} 2)
    list(GET AIP_HOSTILE_VARIANTS ${index} target)
    math(EXPR define_index "${index} + 1")
    list(GET AIP_HOSTILE_VARIANTS ${define_index} define)

    add_library(${target} MODULE aip_hostile_plugin.cpp)
    target_compile_definitions(${target} PRIVATE ${define})
    target_link_libraries(${target} PRIVATE aip_common)

    set(bundle "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${target}.vst3")
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        SUFFIX ".vst3"
        LIBRARY_OUTPUT_DIRECTORY "${bundle}/Contents/x86_64-win")

    string(TOUPPER ${target} upper)
    set(${upper}_PATH "${bundle}" PARENT_SCOPE)
endforeach()
