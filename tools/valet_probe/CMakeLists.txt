# valet_probe -- console client that attaches to the deployed, unmodified APO as a protocol v1
# valet (design_doc.md sec. 7.3). The real-hardware counterpart to the conformance harness.

add_executable(valet_probe main.cpp)

target_link_libraries(valet_probe PRIVATE
    # --plugin runs a real VST3 chain against the real APO; without it nothing from engine/ is
    # reached, but the target is linked either way.
    aip_engine
    aip_ipc
    aip_protocol
    # --scan drives the parent half of scanner/ by hand, which is the only way to point it at a
    # real machine's plugin population rather than at fixtures.
    aip_scanner
    # The allocation hooks must be in the executable itself for the detector to observe
    # anything (sec. 7.4.6); running the probe with them live is a cheap real-world check.
    aip_rt_hooks
)

# --scan starts aip_scan; nothing links it, so nothing would otherwise build it first.
add_dependencies(valet_probe aip_scan)
