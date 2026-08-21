# The Steinberg VST3 SDK, fetched as a pinned release archive (design_doc.md sec. 6.3.2).
#
# Every line below exists because of a trap in sec. 6.3. Read that section before changing any
# of it -- none of these fail in a way that points at its cause.
#
# Refreshing the pin: `https://www.steinberg.net/vst3sdk` answers 302 with the current archive
# URL; download it, `sha256sum` it, and update both values here. There are no GitHub release
# assets for this SDK, and the git repository must not be used (sec. 6.3.2).

include_guard(GLOBAL)
include(FetchContent)

set(AIP_VST3_SDK_VERSION "3.8.1 build 84 (2026-08-11)")
set(AIP_VST3_SDK_URL
    "https://download.steinberg.net/sdk_downloads/vst-sdk_3.8.1_build-84_2026-08-11.zip")
set(AIP_VST3_SDK_SHA256 "64965f1b74e08a6d4087a35af7a716f4dcff5852c66ad7ee13f1c47e79c1ab77")

# The SDK reads these as `option()`s in its own scope. CMP0077 is NEW here (our floor is 3.28
# and the SDK's is 3.25), so a plain normal variable wins and `option()` becomes a no-op --
# which is why these are deliberately *not* cache entries.
#
# Contrary to sec. 6.3.5, in 3.8.1 SMTG_ENABLE_VST3_HOSTING_EXAMPLES *does* gate audiohost,
# editorhost and inspectorapp. What it does not gate is `validator`, and `moduleinfotool` is
# gated by SMTG_ADD_VST3_UTILITIES instead. EXCLUDE_FROM_ALL below covers the remainder.
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF)
set(SMTG_ADD_VST3_UTILITIES OFF)
# Hosting needs no plugin-side GUI toolkit, and vstgui4 is a third of the archive.
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF)
# Post-build steps on plugin targets. The validator run needs a built `validator`, and the
# symlink needs administrator rights -- neither is wanted for our test fixture plugin.
set(SMTG_RUN_VST_VALIDATOR OFF)
set(SMTG_CREATE_PLUGIN_LINK OFF)

FetchContent_Declare(vst3sdk
    URL "${AIP_VST3_SDK_URL}"
    URL_HASH "SHA256=${AIP_VST3_SDK_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Mandatory: the archive root holds vst3sdk/ and VST3_Project_Generator/ and no
    # CMakeLists.txt. Without this, FetchContent populates but silently skips
    # add_subdirectory, and the failure surfaces as a missing *header* (sec. 6.3.3).
    SOURCE_SUBDIR vst3sdk
    # Keeps the SDK's own targets out of `all`. Only what we actually link gets built, which is
    # the cheap half of the sec. 6.3.5 problem: `validator` is still declared, but never built.
    EXCLUDE_FROM_ALL
    # Marks the SDK's interface include directories as SYSTEM so its headers do not have to
    # survive our /W4 /permissive-.
    SYSTEM
)

FetchContent_MakeAvailable(vst3sdk)

if(NOT TARGET sdk_hosting)
    message(FATAL_ERROR
        "The VST3 SDK populated but sdk_hosting does not exist. This is the sec. 6.3.3 failure: "
        "check that SOURCE_SUBDIR still names the directory holding the SDK's CMakeLists.txt.")
endif()

# smtg_create_public_sdk_hosting_target() omits the platform module loader by design; every host
# adds it to its own sources. Omitting it produces one LNK2019 on VST3::Hosting::Module::create
# and no other diagnostic (sec. 6.3.4). Exported as a variable rather than bolted onto
# sdk_hosting so the ownership stays visible at the consuming target.
set(AIP_VST3_MODULE_LOADER_SOURCES
    "${vst3sdk_SOURCE_DIR}/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp")

set(AIP_VST3_SDK_ROOT "${vst3sdk_SOURCE_DIR}/vst3sdk")
