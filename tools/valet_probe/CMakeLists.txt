# valet_probe -- console client that attaches to the deployed, unmodified APO as a protocol v1
# valet (design_doc.md sec. 7.3). The real-hardware counterpart to the conformance harness.

add_executable(valet_probe main.cpp)

target_link_libraries(valet_probe PRIVATE
    aip_ipc
    aip_protocol
    # The allocation hooks must be in the executable itself for the detector to observe
    # anything (sec. 7.4.6); running the probe with them live is a cheap real-world check.
    aip_rt_hooks
)
