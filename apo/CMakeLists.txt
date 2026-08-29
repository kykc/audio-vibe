# The APO: a DLL that is loaded into `audiodg.exe` (design_doc.md sec. 7.1).
#
# Two rules govern this target, and both are about the process it ends up in rather than about
# the code:
#
#  1. **Static CRT.** Everything crossing the boundary to the audio engine is a COM vtable or a
#     POD -- `IAudioMediaType` and friends are released through `Release()`, so allocation and
#     deallocation always happen in the same image -- and the SDK's `audiobaseprocessingobject`
#     is a static library that takes whichever CRT its consumer picks. So /MT costs nothing and
#     buys a DLL with no VC redistributable dependency inside a system process. It also makes
#     the `rt/` operator new replacements safe here: with a private CRT heap they affect this
#     image only.
#
#     The consequence is that this target cannot link `aip_ipc`, `aip_rt_hooks` or anything else
#     built /MD for the client. The handful of sources it shares are compiled into it directly,
#     listed below. That is the intended shape rather than a workaround: it keeps one copy of
#     each source and no second abstraction over it.
#
#  2. **A tiny dependency set.** protocol/ (header-only), rt/ (header-only plus one source), a
#     few `ipc/` primitives, and the Windows SDK. No Qt, no VST3 SDK, no yaml-cpp -- nothing that
#     is not already loaded in `audiodg.exe` or that this DLL cannot afford to fault a page for.

set(AIP_APO_SHARED_SOURCES
    # From ipc/: the Win32 primitives protocol v1 is built out of. Compiled here rather than
    # linked because of the /MT rule above.
    ${CMAKE_SOURCE_DIR}/ipc/src/manual_event.cpp
    ${CMAKE_SOURCE_DIR}/ipc/src/shared_mapping.cpp
    # From rt/: the violation detector's global operator new/delete. Compiled into RelWithDebInfo
    # only in effect -- with AIP_RT_CHECKS off the translation unit is empty (sec. 7.4.6) -- and
    # it is what lets `APOProcess` be held to the sec. 7.4.3 acceptance criterion inside audiodg.
    ${CMAKE_SOURCE_DIR}/rt/src/alloc_hooks.cpp
)

add_library(aip_apo SHARED
    src/audio_ipc_apo.cpp
    src/buffer_king.cpp
    src/dll_main.cpp
    src/registry.cpp
    src/settings.cpp
    src/aip_apo.def
    ${AIP_APO_SHARED_SOURCES}
)

target_include_directories(aip_apo
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # Headers only -- the sources above are compiled in, not linked.
        ${CMAKE_SOURCE_DIR}/ipc/include
)

target_link_libraries(aip_apo
    PRIVATE
        aip_common
        aip_protocol
        aip_rt
        audiobaseprocessingobject   # CBaseAudioProcessingObject, RegisterAPO/UnregisterAPO
        # Not optional, and not obvious: `CBaseAudioProcessingObject::IsFormatTypeSupported`
        # calls `CreateAudioMediaTypeFromUncompressedAudioFormat`, which lives here rather than
        # in the base APO library that references it. Symptom without it is one LNK2019 naming a
        # function this project never calls.
        audiomediatypecrt
        # Also not optional. `aporegistration.obj` inside the base APO library reaches strsafe's
        # `StringVPrintfWorkerW`, which calls `_vsnwprintf` -- and the UCRT made that an inline
        # function, so nothing defines the symbol any more. The predecessor worked around it with
        # a dead `if (false) { _vsnwprintf(NULL, 0, NULL, NULL); }` in `DllRegisterServer` and a
        # comment reading "Fix for msvc14, this function is not inline any more". This is the
        # supported form of the same fix.
        legacy_stdio_definitions
        avrt                        # AvSetMmThreadCharacteristics (sec. 4.6)
        ole32                       # PropVariantClear
        advapi32                    # the registry writes DllRegisterServer makes
)

# `audiomediatypecrt.lib` carries a `/DEFAULTLIB:atls.lib` directive it does not need: dumping its
# symbols shows no undefined ATL reference at all, so the directive is a leftover from however
# Microsoft built it. Honouring it would make the ATL component of Visual Studio a build
# prerequisite for this repository -- it is not installed on this machine, nor on the CI node,
# and neither has any other use for it. Excluded rather than satisfied. If a future SDK really
# does start needing ATL here, the failure is a loud unresolved external, not silence.
target_link_options(aip_apo PRIVATE "/NODEFAULTLIB:atls.lib")

set_target_properties(aip_apo PROPERTIES
    OUTPUT_NAME aip_apo
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_sources(aip_apo PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
    FILES
        include/aip/apo/buffer_king.h
        include/aip/apo/settings.h
)

# What Explorer's Details tab and Task Manager show about this file (cmake/version.cmake). The
# version in it is the one the About box quotes, resolved on the same build.
aip_add_version_resource(aip_apo DESCRIPTION "VibeAudio audio processing object")
