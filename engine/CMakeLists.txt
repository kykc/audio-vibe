# The VST3 host (design_doc.md sec. 7.1). Control-thread work -- module loading, instantiation,
# activation, chain building -- plus the one small class the audio thread reaches, ChainProcessor.

add_library(aip_engine STATIC
    src/chain_processor.cpp
    src/component_handler.cpp
    src/engine.cpp
    src/output_meter.cpp
    src/plugin_chain.cpp
    src/plugin_instance.cpp
    src/plugin_module.cpp
    # smtg_create_public_sdk_hosting_target() omits the platform module loader by design; every
    # host adds it to its own sources. Symptom of omitting it: one LNK2019 on
    # VST3::Hosting::Module::create and nothing else (sec. 6.3.4).
    ${AIP_VST3_MODULE_LOADER_SOURCES}
    # Same class of omission, not recorded in sec. 6.3: MemoryStream lives in
    # public.sdk/source/common but sdk_common's source list does not include it, so a host that
    # transfers component state to the controller has to compile it itself.
    ${AIP_VST3_SDK_ROOT}/public.sdk/source/common/memorystream.cpp
)

target_include_directories(aip_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(aip_engine
    PUBLIC
        aip_common
        aip_ipc
        aip_protocol
        aip_rt
        sdk_hosting
)

if(MSVC)
    # The SDK's headers are pulled in through our own targets, where /W4 /permissive- applies.
    # FetchContent's SYSTEM keyword marks them /external:I; this is the half that makes that
    # mean anything. Our own headers are unaffected -- they are not external.
    target_compile_options(aip_engine PUBLIC /external:W0)
endif()

target_sources(aip_engine PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/aip/engine/audio_thread.h
        include/aip/engine/chain_processor.h
        include/aip/engine/component_handler.h
        include/aip/engine/engine.h
        include/aip/engine/output_meter.h
        include/aip/engine/plugin_chain.h
        include/aip/engine/plugin_instance.h
        include/aip/engine/plugin_module.h
        include/aip/engine/stream_format.h
)
