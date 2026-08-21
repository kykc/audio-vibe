# Header-only C++20 library: protocol v1 (design_doc.md sec. 4), shared by the client, the future
# APO, and the conformance tests. Keep it free of Windows headers and of any dependency.

add_library(aip_protocol INTERFACE)

target_include_directories(aip_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(aip_protocol INTERFACE aip_common)

target_sources(aip_protocol INTERFACE
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/aip/protocol/header_access.h
        include/aip/protocol/layout.h
        include/aip/protocol/planar.h
)
