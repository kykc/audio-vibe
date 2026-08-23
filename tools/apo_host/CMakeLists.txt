# apo_host -- drives a real APO DLL with audiodg.exe out of the loop. See src comments.
#
# Built /MD like the rest of the client side. It never links the APO, only loads it at run time,
# so the /MT boundary in apo/ does not reach here.

# A small library rather than everything in the executable, so the signal bank's validation rules
# can be unit-tested. They are the part with real rules in them -- a tone above Nyquist is not the
# tone that was asked for -- and the part a future generator will be added beside.
add_library(aip_signal STATIC signal_source.cpp)
target_include_directories(aip_signal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aip_signal PUBLIC aip_common)

add_executable(apo_host main.cpp)

target_link_libraries(apo_host PRIVATE
    aip_common
    aip_protocol      # the CLSIDs it tries, and the object-name construction it prints
    aip_signal
    audiomediatypecrt # CreateAudioMediaTypeFromUncompressedAudioFormat
    avrt              # the promoted block thread
    ole32             # CoInitializeEx, CoTaskMemAlloc, CLSIDFromString
)

# Same stale directive as in apo/: audiomediatypecrt.lib asks for atls.lib and does not use it.
target_link_options(apo_host PRIVATE "/NODEFAULTLIB:atls.lib")

# The APO is loaded at run time, so nothing links it and nothing would otherwise build it first
# -- and the default `--dll` is "beside this executable", which has to actually be true or the
# tool's first run is a 126 with no explanation. Copied rather than redirected because the DLL's
# own output location is where `regsvr32` is pointed at, and that should not move.
add_dependencies(apo_host aip_apo)
add_custom_command(TARGET apo_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:aip_apo> $<TARGET_FILE_DIR:apo_host>
    COMMENT "Staging aip_apo.dll beside apo_host")

# It creates Global\ kernel objects (design_doc.md sec. 4.2), which needs SeCreateGlobalPrivilege.
# Asking for elevation in the manifest turns "runs and silently creates nothing" into a prompt.
set_target_properties(apo_host PROPERTIES LINK_FLAGS
    "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
