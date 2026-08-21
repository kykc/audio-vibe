# Client-side IPC: the protocol v1 valet, its promoted audio thread, and the control-thread
# supervisor that owns them (design_doc.md sec. 7.1). Together with protocol/ this is the whole
# audio path, so it stays deliberately small and reviewable (sec. 7.4.6, item 4).

add_library(aip_ipc STATIC
    src/buffer_valet.cpp
    src/endpoints.cpp
    src/manual_event.cpp
    src/shared_mapping.cpp
    src/thread_priority.cpp
    src/valet_supervisor.cpp
    src/valet_thread.cpp
)

target_include_directories(aip_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(aip_ipc
    PUBLIC
        aip_common
        aip_protocol
        aip_rt
    PRIVATE
        avrt       # AvSetMmThreadCharacteristics / AvSetMmThreadPriority (sec. 4.6)
        ole32      # MMDevice enumeration, PROPVARIANT
)

target_sources(aip_ipc PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/aip/ipc/buffer_valet.h
        include/aip/ipc/endpoints.h
        include/aip/ipc/manual_event.h
        include/aip/ipc/null_dacl.h
        include/aip/ipc/shared_mapping.h
        include/aip/ipc/thread_priority.h
        include/aip/ipc/valet_supervisor.h
        include/aip/ipc/valet_thread.h
)
