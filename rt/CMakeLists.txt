# Real-time support: the violation detector (design_doc.md sec. 7.4.6) and the lock-free SPSC
# transport (sec. 7.4.2). Header-only apart from the global allocation hooks.

add_library(aip_rt INTERFACE)

target_include_directories(aip_rt INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aip_rt INTERFACE aip_common)

target_sources(aip_rt INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/aip/rt/mutex.h
        include/aip/rt/realtime_guard.h
        include/aip/rt/spsc_queue.h
)

# OBJECT, not STATIC: replacement operator new/delete must land in the final image
# unconditionally, and a static library's members are only pulled in on demand.
add_library(aip_rt_hooks OBJECT src/alloc_hooks.cpp)
target_link_libraries(aip_rt_hooks PUBLIC aip_rt)
