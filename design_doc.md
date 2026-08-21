# System-wide audio processing utility -- rewrite design document

**Status:** accepted (stack + toolchain). Implementation not started.
**Date:** 2026-08-21
**Predecessor:** `D:\automatl\audio-ipc` (`TomatlVst.sln`), last commit `1a1d3ea`.

---

## 1. Purpose and scope

The predecessor project provides system-wide audio compensation/processing on Windows by
inserting a user-mode Audio Processing Object (APO) into the Windows audio engine and
forwarding every audio block to a userspace host process that runs a chain of audio plugins.

This document covers:

- **Stage 0** -- analysis of the existing solution and verification of the stated motivations.
- **Stage 1** -- technology stack decision.
- **Stage 1.5** -- toolchain, dependency and packaging decisions.

It also freezes the **v1 IPC protocol** as a normative specification, so that the userspace
client and the APO can be rewritten independently and sequentially.

Out of scope for this document: the staged porting plan, UI design, and DSP feature set.

### 1.1 Rewrite motivations

Carried over from the predecessor, verified in sec. 3:

1. Originally written against .NET 2 and increasingly painful to maintain.
2. Mixture of managed C# and C++/CLI.
3. Written as a VST2 host; VST2 is out of support and current Windows plugins ship VST3.
4. APO registration nomenclature changed since Windows 7; the project still uses the
   deprecated `FX` registry entries.
5. UI/UX is limited by Windows Forms.

### 1.2 Guiding constraints

- **Windows-exclusive.** APO and VST3 both make this unavoidable. No cross-platform budget.
- **Preserve the APO <-> userspace data exchange protocol.** This is what allows the rewrite to
  proceed in independent stages. See sec. 4.
- **No web UI.** Multi-process browser runtimes with hundreds of MiB of RSS are a poor fit for a
  background audio utility, and wiring native VST3 plugin editors into a web view is worse.
  Stated as a preference, treated as a decision (see sec. 5.4).
- **Modern UX expectations are mandatory:** per-monitor DPI (including mixed-DPI multi-monitor),
  dark theme, accessible native controls.
- **The audio processing thread is always real-time safe.** No heap allocation, no locking, no
  I/O, no unbounded operations -- in *our* code, without exception. This is a hard rule, not a
  goal. See sec. 7.4 for the normative list and the single sanctioned exception.

### 1.3 Rewrite order

**The userspace client is rewritten first.** The existing APO stays deployed and unmodified;
the new client speaks protocol v1 as a `BufferValet`. This gives a working system at the end of
the first stage with no changes to the kernel-adjacent, hard-to-debug half.

Consequences: the APO-side decisions in sec. 8 are deferred and do **not** block client work.

---

## 2. Existing solution inventory

12 projects, ~13,600 lines of hand-written source (excluding vendored dependencies).

| Project | Language | Role |
|---|---|---|
| `AudioIpcApo` | C++ (no CLR) | The APO. Registers in the GFX slot, aggregates a child APO, performs a blocking rendezvous with userspace |
| `TomatlAudioIpc` | C++ headers | IPC protocol: `AudioIpc.h` (Win32 primitives), `FastStream.h` (`BufferKing`/`BufferValet`) |
| `TomatlVst` | C++ | VST2.4 host, `VstStream` worker thread, `spsc_queue` for control requests |
| `TomatlVstInterop` | **C++/CLI** | 583-line managed/native marshalling bridge |
| `DeRack` | C# WinForms | Main GUI -- one tab per endpoint, plugin rack |
| `Tomatl` | C# | Plot/PEQ custom controls, MMDevice enumeration, APO registration, package model, JSON storage |
| `Config` | C# WinForms | Configurator (endpoint enable/disable) |
| `Cli` | C# | `write-mmd`, `build-package` |
| `Installer` | C# WinForms | Homebrew installer |
| `Tomatl.Local` | C# | Branding strings |
| `ParametricEqVst` | C++/CLI | In-house parametric EQ as a VST2 plugin |
| `DebugStream` | C++ | Loopback test harness -- a fake `BufferKing` that measures round-trip latency |
| `CppTest` | C++ | Scratch `main()` |

### 2.1 Data flow

```
  Application audio
        |
        v
  Windows audio engine (audiodg.exe)
        |
        |  IAudioProcessingObjectRT::APOProcess   [real-time thread]
        v
  AudioIpcApo  --- optional: child APO (original OEM GFX APO) chained first
        |
        |  BufferKing::dispatchCommandment  -- synchronous rendezvous, blocks RT thread
        |     shared memory + two manual-reset events, per endpoint
        v
  DeRack.exe  (BufferValet, via TomatlVstInterop)
        |
        v
  VstStream worker thread -> VstHost -> VST2 plugin chain
        |
        |  writes processed audio back into the same shared buffer
        v
  returns to APOProcess, which re-interleaves and hands the block onward
```

The design has **zero added latency**: the plugin chain runs synchronously on the audio
engine's own clock. That is the virtue of the approach and simultaneously its principal
liability (sec. 9.1).

### 2.2 Registration mechanism

`Tomatl.MMDevice.DeviceAction.performApoExchange` writes, per render endpoint under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{guid}\FxProperties`:

- `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2` (GFX / post-mix effect CLSID)
  := `{B6A6A861-A99F-4F00-B636-657F38F353E9}` (this project's APO)
- `OriginalGfxApo` := the previous value of the above (custom, non-standard value name)

To do this it takes ownership of the endpoint key and replaces its DACL with one granting
`BUILTIN\Administrators` full control. The original owner is never restored -- the restore code
exists but is commented out.

At runtime `AudioIpcApo::Initialize` reads `OriginalGfxApo`, and if it names a valid CLSID,
`CoCreateInstance`s it and chains it ahead of itself. **This feature is dropped in the rewrite**
(sec. 8.3).

---

## 3. Fact verification

### 3.1 ".NET 2" -- outdated description, conclusion stands

No project targets .NET 2 today. Actual current targets:

| Target framework | Projects |
|---|---|
| .NET Framework 4.8 | `Tomatl`, `Cli`, `Installer`, `Tomatl.Local` |
| .NET Framework 4.7.2 | `DeRack`, `Config` |
| .NET Framework 4.5.2 | the C++/CLI projects |

The framework was migrated upward over the project's life (commit `943e9e1`, "migrated to msvc14
and .net 4.5.2"). The real maintenance burden is the *combination*: .NET Framework + Windows
Forms + C++/CLI + MSVC toolset `v141` + `WindowsTargetPlatformVersion 8.1`.

**Verdict:** the motivation is valid; the description should be restated as "legacy .NET
Framework / Windows Forms stack".

### 3.2 Managed + C++/CLI fusion -- confirmed

`TomatlVstInterop` (583 lines, `CLRSupport=true`) and `ParametricEqVst` (`CLRSupport=true`).
The project's own initial commit message describes it as "insane native/managed fusion".

### 3.3 VST2 -- confirmed, and licensing has improved dramatically

The host builds against a vendored VST 2.4 SDK (`Dependencies/VstSdk24`,
`pluginterfaces/vst2.x/aeffect.h` etc.). Steinberg terminated VST2 licensing in 2018 and no
longer distributes the SDK.

**New, material fact:** the VST3 SDK was **relicensed to MIT** with version 3.8 (announced
November 2025). There is no longer a GPLv3-or-proprietary dual license, no signed Steinberg
developer agreement, and no source-disclosure obligation. This removes the largest historical
obstacle to a VST3 rewrite.

### 3.4 APO nomenclature -- confirmed, but migration is not a straightforward win

The modern and legacy property IDs under `FxProperties`, all with GUID
`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}`:

| Slot | Property ID | Status |
|---|---|---|
| SFX (stream effect) | `,5` | modern |
| MFX (mode effect) | `,6` | modern |
| EFX (endpoint effect) | `,7` | modern |
| LFX (local -- legacy name for SFX) | `,1` | deprecated |
| GFX (global -- legacy name for MFX) | `,2` | deprecated -- **currently used by this project** |

Three findings that complicate "just migrate to the modern slots":

1. **Windows prefers modern over legacy when both are configured.** Writing both as a
   belt-and-braces measure does not work; it is strictly either/or.
2. **The modern slots are reportedly broken on Windows 11 24H2 for third-party APOs.** Multiple
   independent reports (including Equalizer APO's issue tracker and discussion forums) state
   that SFX/MFX and SFX/EFX registration has no effect on 24H2 while LFX/GFX continues to work.
   The deprecated slot is presently the *more reliable* one.
3. **Microsoft's modern guidance does not target this deployment model.** The Windows 11 APO
   APIs (`IAudioSystemEffects3`, the CAPX settings/logging/notification/threading frameworks,
   HLK validation) are written for APOs that ship *inside a signed audio driver package*.
   Third-party injection via `FxProperties` is unsupported-but-working in both the legacy and
   modern slots, exactly as it has always been.

Additional operational facts: there is **no code-signing requirement** for the APO DLL; the
Windows "audio enhancements" toggle gates the effect chain; and audio driver reinstallation
wipes these registry values.

**Verdict:** the motivation is factually correct, but the remedy is not "migrate to modern".
See sec. 8.2.

### 3.5 Windows Forms UX ceiling -- confirmed

The repository carries a `Dependencies/win-forms-pm-dpi` submodule directory and three commits
dedicated solely to DPI firefighting (`b252607`, `6913cfa`, `7002bdd`). A dark theme would
require owner-drawing essentially every control.

### 3.6 Summary

| Claim | Verdict |
|---|---|
| 1. .NET 2, painful to support | Framework version outdated (now 4.7.2/4.8); underlying motivation valid |
| 2. C# + C++/CLI mixture | Confirmed |
| 3. VST2 host, VST2 unsupported | Confirmed; VST3 SDK is now MIT, which materially helps |
| 4. Deprecated FX registry entries | Confirmed; but modern slots are less reliable today |
| 5. WinForms limits UI/UX | Confirmed |

### 3.7 Additional defects found during analysis

Not part of the stated motivations, but relevant to the rewrite:

1. **The APO blocks the audio engine's real-time thread for up to 1000 ms.**
   `BufferKing::dispatchCommandment` waits on the `KING` event with the timeout configured in
   `open()` (1000 ms). A stalled or slow userspace process stalls *system-wide* audio for up to
   a full second. See sec. 9.1.
2. **Null DACL on every shared object.** `SetSecurityDescriptorDacl(sd, TRUE, NULL, FALSE)` is
   used for the file mapping, both events and the mutex. Any process on the machine can open
   them, take over the stream (the `valetId` steal is by design), inject audio, or wedge
   `audiodg.exe`. See sec. 9.2.
3. **Logic bug in `BufferKing::smartOpen`:**
   `sampleRate != _sampleRate && channelCount != _channelCount` -- the `&&` should be `||`.
   A sample-rate-only change (44.1 -> 48 kHz at constant channel count) leaves a stale
   `sampleRate` in the shared header.
4. **Will not compile at modern language levels.** `std::uniform_int` and `std::uniform_real`
   (used in `BufferValet::generateClientId` and `DebugStream`) were removed in C++17.
5. **Registry ownership is taken and never returned** (sec. 2.2).
6. **The vendored `Dependencies/baseaudioprocessingobject.h` is no longer needed.** Verified on
   this machine: Windows SDK **10.0.26100** ships
   `Include/10.0.26100.0/um/baseaudioprocessingobject.h` and
   `Lib/10.0.26100.0/um/{x64,arm64,x86}/audiobaseprocessingobject.lib`. No WDK is required,
   and **ARM64 is available at no additional cost**.
7. **Plugin probing happens in-process**, so a malformed plugin can hang or crash the GUI.
8. **No automated tests.** `DebugStream` implements the right idea (a synthetic `BufferKing`
   measuring round-trip latency) but is a manual, interactive console program.

---

## 4. Protocol v1 -- normative specification

This section is **frozen**. The rewritten client must interoperate with the *existing*
unmodified APO, and a future rewritten APO must interoperate with the rewritten client.
Deviations are defects. Improvements are deferred to sec. 9.1.

Derived from `TomatlAudioIpc/FastStream.h` and `AudioIpcApo/AudioIpcApo.cpp` at commit `1a1d3ea`.

### 4.1 Roles

- **King** -- the APO, inside `audiodg.exe`. Creates all objects. Producer of unprocessed audio.
- **Valet** -- the userspace client. Opens existing objects. Consumer/processor.

Exactly one valet may be attached to a given endpoint at a time. A newly arriving valet
*displaces* the incumbent; this is intentional.

### 4.2 Named objects

One set per render endpoint. Let `GUID` be the endpoint GUID as returned by
`PKEY_AudioEndpoint_GUID`, used verbatim -- including braces -- for object names. (The APO
separately lowercases a copy for registry lookups only; that does not affect these names.)

```
base    = L"Global\\TOMATL.AUDIO.IPC." + GUID
mapping = base                  file mapping, 1 MiB, PAGE_READWRITE
event   = base + L".KING"       manual-reset, created SIGNALED
event   = base + L".VALET"      manual-reset, created NON-SIGNALED
```

Notes:

- Size is exactly `1 * 1024 * 1024` bytes. The `MAIN_SIZE` / `MAIN_NAME` / `UTILITY_*` /
  `STREAM_INFO_MUTEX_NAME` / `SRV_WAIT` / `CLT_WAIT` macros in `AudioIpc.h` are dead legacy and
  are **not** part of v1.
- Both events are **manual-reset**.
- Objects are created with a null DACL (sec. 9.2).
- The APO creates the mapping with `CreateFileMapping(INVALID_HANDLE_VALUE, ...)`; the valet
  also calls `CreateFileMapping` with the same name and size, which attaches to the existing
  mapping.

### 4.3 Shared memory layout

Little-endian, packed, no padding. `ULONG` and `int` are 4 bytes.

| Offset | Type | Field | Written by |
|---|---|---|---|
| 0 | `ULONG` | `valetId` -- 0 means "no client attached" | valet (claim), king (eviction) |
| 4 | `ULONG` | `sampleRate` | king |
| 8 | `ULONG` | `channelCount` | king |
| 12 | `int` | `size` -- **total** sample count, i.e. `frames * channelCount` | king |
| 16 | `float[size]` | audio payload, **de-interleaved (planar)** | king (input), valet (output) |

Payload layout is planar with `perChannel = size / channelCount` frames per channel:
sample `s` of channel `c` lives at float index `c * perChannel + s`.

The payload buffer is **shared in place**: the valet reads the king's input and writes its
output into the same memory. There is no separate output region.

Maximum payload: `(1 MiB - 16) / 4 = 262,140` floats, i.e. 32,767 frames at 8 channels.
Real block sizes are three orders of magnitude smaller.

Sample format is 32-bit float. The APO declares
`APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE`.

### 4.4 Handshake and per-block exchange

**Valet attach:**

1. Open `<base>.KING` and `<base>.VALET` with `OpenEvent(EVENT_ALL_ACCESS, TRUE, name)`.
   If either does not exist, the endpoint is not active -- fail and retry later.
2. Attach to the mapping.
3. Generate a **nonzero** random 32-bit `valetId`.
4. `ResetEvent(VALET)`.
5. Write `valetId` at offset 0. The valet is now attached.

**King, per audio block** (`APOProcess`):

1. Read `valetId` from offset 0.
2. If `valetId == 0`: copy input to output unchanged. Done.
3. Write `sampleRate`, `channelCount`, `size`, and the de-interleaved input payload.
4. `ResetEvent(KING)`; `SetEvent(VALET)`.
5. `WaitForSingleObject(KING, 1000)`.
   - **Signaled:** read the payload back, re-interleaving into the output buffer.
   - **Timeout:** write `0` to `valetId` (evict the valet) and copy input to output unchanged.

**Valet, per audio block:**

1. `WaitForSingleObject(VALET, timeout)` -- the reference implementation passes `INFINITE`.
2. Read `valetId` at offset 0. **If it does not match this valet's own id, the stream has been
   taken over by another client (`Stolen`) -- the valet must detach.**
3. Read `sampleRate`, `channelCount`, `size`. Compute `blockSize = size / channelCount`.
4. Process the planar payload in place.
5. `ResetEvent(VALET)`; `SetEvent(KING)`.
   To detach cleanly, write `0` to `valetId` before this step.

### 4.5 Lifecycle

The king calls `smartOpen(sampleRate, channelCount)` from `LockForProcess` and `smartClose()`
from its destructor. On open it sets `KING` and clears `VALET`; if it created the mapping fresh
it zeroes `valetId`. On close it clears both events and releases all handles -- the valet then
sees its waits fail and must re-attach.

Note the `smartOpen` bug in sec. 3.7.3: a sample-rate-only format change does not trigger a
reopen, so a stale `sampleRate` can be observed. **The new client must tolerate this** by
reading `sampleRate` and `channelCount` from the header on every block rather than caching them
across blocks.

### 4.6 Thread priority

Both sides promote their audio thread. The reference implementation
(`Tomatl::AudioIpc::Priority::promoteCurrentThread`) calls
`SetThreadPriority(GetCurrentThread(), 15)` followed by
`AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex)` and
`AvSetMmThreadPriority(handle, AVRT_PRIORITY_CRITICAL)`.

The new client must do the same on its valet thread. This is behavioural, not wire-level, but
it is required for glitch-free operation.

A thread promoted to `AVRT_PRIORITY_CRITICAL` will starve normal-priority threads if it blocks
on anything they hold. This makes the real-time safety rules in sec. 7.4 a correctness requirement,
not merely a performance preference.

### 4.7 Conformance testing

A protocol conformance harness is a first-class deliverable, replacing the manual
`DebugStream`:

- a synthetic king that drives blocks at a configurable rate/format and asserts round-trip
  latency bounds;
- a synthetic valet, for exercising a rewritten APO later;
- tests for: attach/detach, takeover (`Stolen`), king-side timeout eviction, format change
  mid-stream, planar round-trip fidelity, and the sec. 4.5 stale-`sampleRate` case.

---

## 5. Stack decision

### 5.1 The deciding constraint

VST3 exposes plugin editors through `IPlugView` with platform type `kPlatformTypeHWND`: the
host must supply a **real child window handle**, and should implement
`IPlugViewContentScaleSupport` for DPI. Any UI framework that cannot hand out a genuine HWND
and host a foreign child HWND with correct per-monitor scaling is disqualified, regardless of
how good its app chrome is.

This single requirement, not aesthetics, determines the stack.

### 5.2 Decision

**C++20 / single userspace process / Qt 6 Widgets / VST3 SDK linked directly.**

| Layer | Choice | Rationale |
|---|---|---|
| Userspace host | C++20, MSVC | One language for the whole system, including the future APO. No marshalling layer. |
| GUI | **Qt 6 Widgets** (LGPLv3) | Per-Monitor-v2 aware by default; native `windows11` style with real dark mode; every widget can yield an HWND; `QPainter` is a direct port target for the existing `Plot`/`PeqControl` custom drawing. |
| Plugin editor hosting | `QWindow::fromWinId()` + `QWidget::createWindowContainer()` | The standard Qt technique for embedding a foreign child HWND. Verified working (sec. 10). |
| Plugin format | **VST3 SDK 3.8.1**, MIT | Use `public.sdk/source/vst/hosting` (`module.h`, `PlugProvider`, host classes). |
| Plugin scanning | **Separate short-lived probe process** | Fixes sec. 3.7.7 -- a malformed plugin can no longer take down the GUI. |
| Audio thread | Own valet thread + `AvSetMmThreadCharacteristics("Pro Audio")` | Per sec. 4.6. No audio framework needed. |
| Future APO | C++20, no CLR, **static CRT**, x64 + ARM64 | Static CRT avoids a VC++ redistributable dependency inside `audiodg.exe`. Links the SDK's `audiobaseprocessingobject.lib`. |
| Protocol | Header-only C++20 library, single source of truth | Shared verbatim by client, future APO and the conformance harness. |

**Licensing outcome:** Qt under LGPLv3 (dynamic linking, Qt DLLs shipped alongside) plus a
MIT-licensed VST3 SDK permits releasing this project's own source under MIT.

### 5.3 Runner-up: C++ core + .NET 10 WPF shell

A flat C-ABI DLL for the engine, with a WPF front end. Worth revisiting if the GUI ambition
grows beyond what Qt Widgets makes pleasant:

- `HwndHost` is the most battle-tested foreign-HWND host on Windows.
- .NET 9+ WPF ships a first-party Fluent theme with integrated light/dark and system accent.
- Source-generated `[LibraryImport]` P/Invoke over a flat C ABI is nothing like C++/CLI.

Rejected because it reintroduces a language boundary and a second build system -- precisely
motivation sec. 1.1.2 -- and adds a .NET runtime deployment.

### 5.4 Rejected alternatives

| Option | Reason for rejection |
|---|---|
| **WinUI 3 / Windows App SDK** | Controls are windowless composition surfaces; there is no supported `HwndHost` equivalent. Hosting a plugin's child HWND leads to airspace defects. Disqualified by sec. 5.1. |
| **Web UI** (Electron/Tauri/webview) | Multi-process RAM footprint is inappropriate for a background audio utility, and the plugin-editor HWND problem is unsolved -- a native side-window would still be required, paying both costs. Per sec. 1.2. |
| **Dear ImGui** | Acceptable for a debug overlay; wrong for accessibility, IME, and per-monitor text quality. |
| **Rust** | VST3 hosting bindings are thin; a COM APO deriving from `CBaseAudioProcessingObject` is C++-shaped work. |
| **JUCE 8** | Genuinely attractive -- hosting, out-of-process scanning and plugin state for free. Rejected because its non-native look fights sec. 1.2, and it imposes AGPLv3-or-commercial where the VST3 SDK is now MIT. **Reconsider only if VST3 hosting proves harder than projected.** |

### 5.5 VST2

**Dropped.** If a VST2-only plugin is ever genuinely needed, a third-party VST2->VST3 wrapper
can be used, or a narrow in-house wrapper written against the vendored 2.4 SDK in the
predecessor repository. Not redistributable, so not shipped.

---

## 6. Toolchain and dependencies

### 6.1 Decision

| Concern | Choice |
|---|---|
| Environment / tools | **pixi** -- `pixi.toml` and `pixi.lock` committed to the repository |
| pixi packages | `qt6-main`, `cmake`, `ninja`, `vs2022_win-64`, `catch2` |
| Compiler | Local MSVC v143, activated automatically by `vs2022_win-64` |
| Build system | CMake (4.x from pixi) with `CMakePresets.json` |
| Generator | **Ninja Multi-Config** |
| Primary configuration | **`RelWithDebInfo`** (see sec. 6.4) |
| Third-party source deps | **`FetchContent` only.** No git submodules, no vcpkg, no Conan |
| VST3 SDK | `FetchContent` from the pinned official release archive + `URL_HASH` |
| Windows SDK | 10.0.26100 (already installed) |
| Architectures | x64 primary, ARM64 supported (sec. 3.7.6) |
| Installer | **WiX v7** (.NET tool / MSBuild SDK) |
| Qt deployment | `windeployqt6` / `qt_generate_deploy_app_script` |
| Tests | Catch2 v3 + `ctest` |
| Hygiene | `clang-format`, `clang-tidy`, `/W4 /permissive-`, `/analyze` on the APO, ASCII-only sources (sec. 6.6) |

Rationale for pixi over vcpkg/Conan: the whole toolchain -- compiler activation, Qt, CMake,
Ninja, test framework -- comes from one lockfile, reproducibly, with no source builds. Verified
end to end in sec. 10. `vcpkg`'s `qtbase` port is a multi-hour source build; the official Qt
installer is not lockfile-reproducible.

Rationale for `FetchContent` over submodules: git is a version control system, not a package
manager. A pinned archive URL with a content hash is *more* reproducible than a git tag, since
tags are mutable.

**MSVC is not redistributable** and is therefore not provided by pixi. `vs2022_win-64` only
*activates* a local install. **Visual Studio 2022 (or 2026) with the C++ workload remains a
documented machine prerequisite.**

### 6.2 Machine prerequisites

1. Visual Studio 2022 or 2026 with the Desktop C++ workload.
2. Windows SDK 10.0.26100 or later.
3. pixi.
4. **Long path support enabled** -- see sec. 6.3.1. Both of the following:
   - `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1`
   - `git config --system core.longpaths true`

### 6.3 Known build traps

These were all discovered empirically (sec. 10) and each fails in a way that does not obviously
point at its cause.

#### 6.3.1 MAX_PATH -- the most severe

The VST3 SDK contains paths such as
`public.sdk/samples/vst/note_expression_synth_auv3/iOS/Resources/Assets.xcassets/AppIcon.appiconset/apple-icon-120x120.png`
(~120 characters), and the SDK's own CMake generates object paths like
`validator.dir/RelWithDebInfo/__/__/__/source/vst/...`.

Observed failures at a 142-character source root:

- `git`: `error: unable to create file ...: Filename too long`, then
  `fatal: Unable to checkout ... in submodule path 'public.sdk'`
- MSVC: `fatal error C1083: Cannot open compiler generated file: '': Invalid argument`

The identical tree at a ~30-character root builds cleanly.

**Mitigations:** keep the source and build trees shallow; enable long path support per sec. 6.2.4.
Note that `GIT_CONFIG core.longpaths=true` inside `FetchContent_Declare` does **not** help -- it
applies only to the top-level clone, not to submodule clones. This was tested and confirmed
ineffective.

#### 6.3.2 Use the release archive, not the git repository

`GIT_REPOSITORY` pulls seven submodules declared with *relative* URLs, totalling **501 MB** --
including `vstgui4`, `doc` and `tutorials`, none of which a host needs. It is also the primary
trigger for sec. 6.3.1. Restricting `GIT_SUBMODULES` does not help, because `public.sdk` itself
contains the offending long paths.

The official release archive is 246 MB extracted, contains no submodules, and can be pinned by
content hash:

```cmake
FetchContent_Declare(vst3sdk
  URL https://download.steinberg.net/sdk_downloads/vst-sdk_3.8.1_build-84_2026-08-11.zip
  URL_HASH SHA256=64965f1b74e08a6d4087a35af7a716f4dcff5852c66ad7ee13f1c47e79c1ab77
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR vst3sdk
)
```

`https://www.steinberg.net/vst3sdk` redirects (302) to that exact URL, which is how to discover
the current one. There are no GitHub release assets for this repository.

**The hash above is the live pin** (124,219,090 bytes; 221 MB extracted), verified against the
archive on 2026-08-21 and in `cmake/vst3sdk.cmake`. To refresh it: follow the redirect, download,
`sha256sum`, and update both values in that one file.

#### 6.3.3 `SOURCE_SUBDIR` is mandatory

The archive contains a single top-level `VST_SDK/` directory, which CMake's extraction strips, so
the populated source directory holds `vst3sdk/` and `VST3_Project_Generator/` and **no
`CMakeLists.txt`**.
Without `SOURCE_SUBDIR vst3sdk`, `FetchContent_MakeAvailable` populates the content but silently
skips `add_subdirectory`. The `sdk_hosting` target then never exists, and the failure surfaces
as a *missing header* error rather than a missing-target error.

#### 6.3.4 `module_win32.cpp` is deliberately not part of `sdk_hosting`

`smtg_create_public_sdk_hosting_target()` omits the platform module loader by design; every
host application must add it to its own sources. The SDK's own `audiohost` sample does exactly
this. Omitting it produces a single `LNK2019` on `VST3::Hosting::Module::create` and no other
diagnostic.

```cmake
qt_add_executable(app WIN32 main.cpp
  ${vst3sdk_SOURCE_DIR}/vst3sdk/public.sdk/source/vst/hosting/module_win32.cpp)
target_link_libraries(app PRIVATE Qt6::Widgets sdk_hosting)
```

#### 6.3.5 The SDK's CMake is not a well-behaved dependency

In the SDK's root `CMakeLists.txt`, `add_subdirectory(public.sdk/samples/vst-hosting)` and
`add_subdirectory(public.sdk/samples/vst-utilities)` are **unconditional**, so consuming the SDK
via `add_subdirectory` always *declares* the sample targets. A single-file test application
produced 66 of them.

Two corrections established while integrating 3.8.1, both of which reduce the damage
substantially:

- `SMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF` **does** gate `audiohost`, `editorhost` and
  `inspectorapp` in this version -- each of their `CMakeLists.txt` files opens with a test on it.
  What it does not gate is `validator`; `moduleinfotool` is gated by `SMTG_ADD_VST3_UTILITIES`
  instead. With those two options plus `SMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF` and
  `SMTG_ENABLE_VSTGUI_SUPPORT=OFF`, the SDK declares six targets: `base`, `pluginterfaces`,
  `sdk`, `sdk_common`, `sdk_hosting` and `validator`.
- `EXCLUDE_FROM_ALL` on `FetchContent_Declare` (CMake 3.28) keeps the declared-but-unwanted
  targets out of `all`, so only what is actually linked gets compiled. `validator` remains
  declared and is never built.

Together those take the whole-tree build from 66 targets to five compiled libraries.

Two further omissions of the same kind as sec. 6.3.4, each producing link errors that name a
class the consumer never mentions:

- `public.sdk/source/common/memorystream.cpp` is **not** in `sdk_common`. Any host that transfers
  component state to a separated controller must compile it itself.
- `public.sdk/source/vst/vstsinglecomponenteffect.cpp` is **not** in `sdk`, and it happens to hold
  the definitions of `EditController::setEditorState` and `getEditorState`. Omitting it yields
  fifteen `LNK2001`s, most of them for `EditController`.

**Preferred longer-term approach** (sec. 9.6): bypass the SDK's CMake and declare our own static
library over the ~20 translation units that `sdk_hosting` actually lists
(`public.sdk/source/vst/hosting/*.cpp`, `utility/stringconvert.cpp`, `vstinitiids.cpp`) plus
`pluginterfaces` and `base`. For a host this is a small, well-understood surface and gives full
control over flags and build time. Less urgent now that the target count is five.

### 6.4 Qt is Release-only

The conda-forge `qt6-main` package ships **no debug libraries** -- verified: no `Qt6Cored.dll`,
no `Qt6Widgetsd.dll`. Consequently a `Debug` configuration is unavailable: a `/MDd`
application cannot link `/MD` Qt (`_ITERATOR_DEBUG_LEVEL` mismatch, `LNK2038`).

**`RelWithDebInfo` is the primary development configuration** -- `/MD -Zi`, full PDBs, fully
debuggable. This is the configuration used for all verification in sec. 10.

If a genuine debug Qt is ever required, that is the one circumstance that would justify moving
Qt to the official installer or a vcpkg source build.

### 6.5 `QWidget::grab()` does not capture embedded plugin editors

`grab()` renders Qt's own backing store; a foreign child HWND is composited by Windows and
appears blank. Irrelevant for normal operation, but it means editor thumbnails or automated
visual tests over plugin GUIs require `PrintWindow`/`BitBlt` instead.

### 6.6 Source and documentation files are ASCII-only

**Every tracked text file in this repository -- source, headers, CMake, TOML, JSON, Markdown,
including this document -- contains only US-ASCII characters (code points 0x00-0x7F).** Files
are UTF-8 encoded, which for ASCII-only content is byte-identical to ASCII; the encoding is not
the point, the character repertoire is.

Substitutions, given by code point so that this table is itself ASCII:

| Instead of | Write | Note |
|---|---|---|
| U+00A7 section sign | `sec. ` | `sec. 4.3`; `Sec. 4.3` when it starts a sentence |
| U+2014 em dash | `--` | spaced as ` -- ` where the dash separates clauses |
| U+2013 en dash | `-` | |
| U+00B7 middle dot | `/` | a separator between items listed on one line |
| U+2192, U+2194 arrows | `->`, `<->` | |
| U+2265, U+2264, U+00D7 | `>=`, `<=`, `x` | |
| U+2018/19/1C/1D quotes | `'`, `"` | plain apostrophe and plain double quote |
| U+2026 ellipsis | `...` | |
| U+00A0 no-break space | plain space | invisible, and the worst of the set |

**Why.** Every one of these bit us, and none of the failures pointed at its cause:

1. **Test names round-trip through `ctest` and get mangled.** `catch_discover_tests` registers
   each Catch2 test under its name and then re-invokes the executable with that name as a
   filter. A section sign in the name does not survive the trip through `argv` under the console
   codepage, so the filter matches nothing and the test fails with "No tests ran" -- while
   passing perfectly when run directly. Four tests failed this way, and the message says
   nothing about encoding.
2. **MSVC diagnostics, console output and log files are codepage-dependent.** The same string
   renders correctly in one terminal and as mojibake in another, which makes a non-ASCII
   character in an error message actively misleading.
3. **Toolchain robustness.** `clang-format`, `clang-tidy`, `awk`/`grep` in a POSIX shell, and
   Python scripts run with the default Windows codepage each have their own idea of the input
   encoding. Restricting the repertoire removes the whole class of question.
4. **Diff and patch fidelity.** Tools that transport source as text can silently normalise or
   drop characters. During implementation a `\\` in a wide string literal was reduced to `\`,
   turning `L"Global\\TOMATL.AUDIO.IPC."` into `L"Global\TOMATL.AUDIO.IPC."` -- an object name
   that would never match the APO's. That one was caught by MSVC warning C4129 only because the
   result happened to be an invalid escape sequence. A mangled section sign, by contrast,
   produces no diagnostic at all.

The rule is mechanically checkable and the check is cheap: a tracked text file containing a byte
>= 0x80 is a defect. `tests/source_hygiene_test.cpp` walks the source tree on every `ctest` run
and reports the offending file, line, column and byte, so a stray character fails the suite
instead of surfacing later as a mangled test filter.

The one legitimate exception is a string that must contain non-ASCII data *at run time* -- for
example a test fixture exercising an endpoint name with non-Latin characters. Write those as
escape sequences -- `L"\u00e9"`, never the literal e-acute character -- so that the source file
stays ASCII while the runtime value does not.

---

## 7. Target architecture

### 7.1 Components

```
audio-ipc2/
  protocol/        header-only C++20 library -- protocol v1 (sec. 4), the single source of truth
  ipc/             BufferValet implementation, valet thread, thread promotion (sec. 4.6)
  engine/          VST3 host: module loading, plugin chain, state, processing graph
  scanner/         separate executable -- out-of-process plugin probing
  ui/              Qt 6 Widgets shell, plugin rack, editor hosting, EQ/plot widgets
  apo/             (later stage) the rewritten APO
  tests/           Catch2: protocol conformance (sec. 4.7), engine, scanner
  installer/       WiX v7
```

### 7.2 Process model

- **One userspace process** hosts the GUI, the VST3 plugins and their editors, and the valet
  thread. Plugin editors need a UI thread in the same process as their HWND, so splitting the
  GUI from the engine would buy nothing.
- **One short-lived scanner process per scan**, for crash isolation during plugin probing.
- The APO remains, necessarily, inside `audiodg.exe`.

### 7.3 Client-first staging

For the first stage the **existing, unmodified APO stays deployed**. The new client attaches as
a protocol v1 valet. Registration, installation and the APO itself are untouched, which means
the entire kernel-adjacent half of the system is out of scope until the client is working.

### 7.4 Real-time safety rules (normative)

**The audio processing thread must remain real-time safe at all times.** This applies to the
valet thread in the client, to `APOProcess` in the future APO, and to every function either
one calls. It is a hard rule. Code that violates it is defective even if it appears to work.

The threads in scope are:

- the client's **valet thread** -- protocol rendezvous, plugin chain dispatch, any DSP of our own;
- the future APO's **`APOProcess`** and anything it calls;
- any callback either of the above invokes, including our implementations of VST3 host
  interfaces (sec. 7.4.5).

#### 7.4.1 Prohibited on the audio thread

| Category | Specifically forbidden |
|---|---|
| Heap | `malloc`, `free`, `new`, `delete`, `realloc`, and anything that may allocate: `std::vector`/`std::string`/`std::map` growth or construction, `std::function` construction with a non-inlinable capture, `std::shared_ptr` creation, `std::any` |
| Locking | `std::mutex`, `std::shared_mutex`, `std::condition_variable`, `EnterCriticalSection`, SRW locks, `WaitForSingleObject` on a mutex or semaphore, spin locks shared with a non-real-time thread, and any other construct that can invert priority against a normal-priority thread |
| I/O | Filesystem access, registry access, console or file logging, `printf`, iostreams, `OutputDebugString` |
| OS / loader | `LoadLibrary`, `GetProcAddress`, `CoCreateInstance` or any COM activation, thread or process creation, `Sleep`, timer waits |
| Control flow | Throwing exceptions (unwinding can allocate), unbounded loops, recursion without a static depth bound |
| Memory behaviour | First-touch page faults; any buffer not already committed and touched before the stream starts |

#### 7.4.2 Required patterns

- **Preallocate everything** at stream open (`LockForProcess` on the APO side, valet attach on
  the client side), sized from `sampleRate`, `channelCount` and the maximum block size implied
  by sec. 4.3. Touch every page before the first block.
- **Fixed-capacity buffers only** on the audio path. No growth, no reallocation.
- **Control-plane -> audio-thread messaging via a lock-free single-producer/single-consumer
  queue.** The predecessor already did this correctly (`TomatlVst/spsc_queue.h`); keep the
  pattern. Commands (add/remove/reorder/bypass a plugin, gain changes) are enqueued by the UI
  thread and drained by the audio thread with bounded work per block.
- **Audio-thread -> control-plane messaging** likewise: enqueue, never block, never allocate.
  The UI thread polls.
- **Plugin chain mutation never happens on the audio thread.** Build the new chain on the
  control thread, publish it by a single atomic pointer store, and retire the old one on the
  control thread after a safe grace period. The audio thread only ever reads the current
  pointer. This is the most important instance of the general principle in sec. 7.4.3.
- **Bounded work per block.** If a control-plane queue is backlogged, drain a fixed maximum per
  block rather than catching up in one go.

#### 7.4.3 When a violation is unavoidable -- minimise the surface

Some operations cannot be made allocation-free end to end. Adding a VST3 plugin to a chain that
is actively processing is the canonical case: the plugin's own `setupProcessing` and
`setActive` will allocate, and we do not control that.

The rule in these cases is **not** "give up on sec. 7.4.1" -- it is **push the violation as far off
the audio thread as it will go, and shrink what remains to the smallest possible surface**:

1. **Do everything possible on the control thread.** Load the module, instantiate the
   component, `setupProcessing`, `setActive`, restore state, allocate every buffer and touch
   every page -- all before the audio thread learns the object exists.
2. **Hand over a finished, ready-to-run object** through a non-blocking primitive. An SPSC
   queue was used previously and remains a fine choice; atomic pointer publication is another.
   The specific mechanism is not prescribed -- the properties are: no blocking, no allocation,
   no unbounded work on the consumer side.
3. **What executes on the audio thread must be O(1) and allocation-free** -- ideally a pointer
   read and a splice into the chain, nothing more.
4. **Destroy the replaced object on the control thread**, never on the audio thread. Frees are
   as forbidden as allocations (sec. 7.4.1).

If step 3 amounts to more than a pointer swap, the work has not been pushed far enough upstream.
That is a design defect, not a necessary cost.

**Acceptance criterion.** The distinction that governs how much disruption is tolerable:

- **User-initiated transitions** -- adding, removing or reordering a plugin, loading a preset --
  **may produce audible artifacts, and that is acceptable.** The user acted and expects a
  transition; the plugin's own DSP may click or fade while it warms up, which is outside our
  control anyway. We are not obliged to make these sample-accurate or click-free. We *are*
  obliged to keep our own contribution to the disruption minimal and bounded.
- **Steady state** -- no user interaction -- **must be completely inert.** Zero allocations, zero
  frees, no growth in memory footprint, no lock acquisition, no syscalls beyond sec. 7.4.4. A chain
  that has been running untouched for hours must show the same audio-thread allocation count and
  the same resident set as it did one second after it started.

The second bullet is directly testable and should be enforced as such: the detector in sec. 7.4.6
counts audio-thread allocations, and steady-state must be **exactly zero**. A soak test
asserting a flat allocation count and flat RSS over a long idle run is the regression guard.

#### 7.4.4 The sanctioned exception -- protocol synchronisation primitives

The Win32 primitives used by `BufferKing` / `BufferValet` are **deliberately permitted on the
audio thread**. They were selected and vetted for this purpose and are the one carve-out from
Sec. 7.4.1:

- `SetEvent` / `ResetEvent` on the `KING` and `VALET` manual-reset events -- non-allocating
  syscalls with bounded cost, no priority inversion against a lock holder.
- `WaitForSingleObject` on those two events -- this rendezvous *is* the protocol (sec. 4.4) and
  cannot be removed without a v2.
- Reads and writes through the mapped view of the shared section, provided the view was mapped
  and touched at stream open.

Two qualifications:

1. This exception is **exhaustive**. It covers exactly these operations on exactly these
   objects. It is not a general licence to call blocking Win32 APIs, and in particular it does
   **not** extend to `SimpleMutex` / `ThreadSafeContainer` from the predecessor's `AudioIpc.h`
   -- those are dead legacy, are not part of protocol v1 (sec. 4.2), and must not appear on the
   audio path.
2. The king-side `WaitForSingleObject(KING, 1000)` is the **known weak point of v1**, not an
   endorsement of long blocking waits. It is the reason sec. 9.1 exists. Until v2, it stays as
   specified.

#### 7.4.5 Third-party plugin code

We do not control what a VST3 plugin does inside `IAudioProcessor::process`. Plugins allocate,
lock and occasionally touch the filesystem, and no host can prevent it.

Our obligation is that **our half of the call stack is clean**, which means specifically:

- Call `process` directly. Do not wrap it in anything that allocates, locks, or copies through
  a dynamically sized buffer.
- Preallocate and reuse all `ProcessData`, `AudioBusBuffers`, `IParameterChanges` and
  `IEventList` structures. The SDK's `HostProcessData` supports this -- set it up once at stream
  open.
- **Our implementations of VST3 host interfaces called from the audio thread must themselves be
  real-time safe.** `IComponentHandler::performEdit` in particular may legitimately be invoked
  by a plugin from the processing thread: our implementation must enqueue to the UI thread
  lock-free and return, never touch a lock or the heap. Anything that must not run on the audio
  thread -- `restartComponent`, editor notifications, state persistence -- is deferred to the
  control thread through the same queue.
- A misbehaving plugin degrades that plugin. It must not be able to make *our* code violate
  sec. 7.4.1.

#### 7.4.6 Enforcement

`clang`'s `RealtimeSanitizer` (`-fsanitize=realtime`) is the natural tool here but is
unavailable to us, as we build with MSVC (sec. 6.1). Substitutes, in order of value:

1. **A debug-only real-time violation detector.** A thread-local "inside real-time section"
   flag set by an RAII guard at the top of the valet callback, checked by a global
   `operator new` / `operator delete` override and by thin assert-only wrappers around the
   locking primitives we might accidentally reach for. Compiled into `RelWithDebInfo`, compiled
   out of release. This catches the overwhelming majority of real violations cheaply.
2. **The conformance harness (sec. 4.7)** asserting round-trip latency bounds -- catches gross
   violations and regressions, though not rare ones.
3. **Code review against sec. 7.4.1**, treated as a merge gate for anything touching the audio path.
4. Keeping the audio path physically small and reviewable: `protocol/` and `ipc/` are the only
   places these rules are hard to see at a glance, so keep them minimal (sec. 7.1).

---

## 8. Deferred decisions

These are all APO-side and do not block client work (sec. 1.3, sec. 7.3).

### 8.1 Minimum OS version

**Leaning: Windows 11 only.** Windows 11 (build 22000+) unlocks the CAPX frameworks --
`IAudioSystemEffects3`, the settings property store, first-party APO logging, the real-time
work queue -- and would let the APO stop hand-rolling registry access for its settings.
Supporting Windows 10 means retaining the `APOInitSystemEffects2` path indefinitely.

To be decided when APO work begins.

### 8.2 Registration slot policy

**Leaning: GFX (`,2`) only**, on the sec. 3.4 evidence that the modern slots are currently
unreliable for third-party APOs on 24H2. Requires independent verification by the project
owner before being fixed.

Whatever is chosen must be **either/or, never both** (sec. 3.4, finding 1), and should be
accompanied by a post-registration verification step that confirms the APO is actually being
loaded.

### 8.3 Child APO aggregation -- dropped

The predecessor's ability to chain the OEM's original GFX APO (sec. 2.2) is **removed**. It was
never used in practice and the predecessor's own commit history describes it as troublesome
(`47f449a`, "APO aggregation (causes trouble, but works somehow)").

---

## 9. Future improvements

Deliberately deferred to keep protocol v1 frozen (sec. 1.2, sec. 4). Recorded here so they are not
lost.

### 9.1 Protocol v2: bounded real-time blocking

**The single most important improvement.** Protocol v1 lets a stalled userspace process block
the audio engine's real-time thread for up to 1000 ms (sec. 3.7.1), which manifests as a
system-wide audio dropout.

A v2 should:

- add a **version/capability word** to the shared header so v1 and v2 peers can negotiate,
  keeping v1 as the fallback;
- reduce the king-side timeout to a **fraction of the audio period** (single-digit
  milliseconds) with immediate, glitch-free bypass on expiry;
- consider decoupling entirely -- a lock-free ring buffer with an explicit, declared latency
  budget -- trading v1's zero added latency for the audio engine no longer being hostage to a
  userspace process. This is a real trade-off, not a free win, and deserves its own decision.

Note that the 1000 ms wait is the only part of the audio path that sec. 7.4 tolerates rather than
endorses (sec. 7.4.4, qualification 2). Everything else on that path is required to be
unconditionally real-time safe, so this is the single remaining unbounded operation and the
main reason to pursue v2.

### 9.2 Tighten shared object security

Replace the null DACL (sec. 3.7.2) with an explicit one granting access only to the audio service
identity and `BUILTIN\Administrators`. Requires coordinated change on both sides, hence v2.

### 9.3 Migrate to the modern registration slots

Revisit sec. 3.4 once Microsoft fixes third-party SFX/MFX/EFX registration on current Windows 11
builds. Until then GFX is the pragmatic choice.

### 9.4 Restore registry ownership

The installer should restore the original owner and DACL of the endpoint key on uninstall
(sec. 3.7.5).

### 9.5 Adopt CAPX

Contingent on sec. 8.1. Would replace hand-rolled registry settings access, add structured
logging from inside `audiodg.exe` (a significant debuggability win), and provide a real-time
work queue.

### 9.6 Replace SDK CMake consumption with a minimal in-house target

Per sec. 6.3.5.

---

## 10. Verification record

All of the following was executed on the development machine on 2026-08-21 and passed. This
is the evidence base for sec. 5 and sec. 6; it is recorded so the decisions can be re-examined rather
than re-litigated.

**Machine:** Windows 11 build 26200 (25H2) / VS 2022 Enterprise, MSVC 14.44.35207 /
Windows SDK 10.0.26100 / pixi 0.76.2.

**Environment.** `pixi add qt6-main cmake ninja vs2022_win-64` resolved Qt **6.11.2** win-64
with complete CMake config packages, `moc` / `uic` / `rcc`, `windeployqt6`, and
`plugins/styles/qmodernwindowsstyle.dll` (the genuine Windows 11 style), plus CMake 4.4.2 and
Ninja 1.13.2. `vs2022_win-64` activated the local MSVC automatically inside `pixi run` --
`cl.exe` 19.44 resolved from the VS 2022 Enterprise install with no `vcvars` invocation and no
Developer Command Prompt.

**Integration probe.** A Qt Widgets application plus the VST3 SDK 3.8.1 obtained via
`FetchContent` (archive URL, `SOURCE_SUBDIR`), linking `sdk_hosting` and compiling
`module_win32.cpp`, with a foreign child HWND embedded via `QWindow::fromWinId()` ->
`QWidget::createWindowContainer()`. Configured, built (`RelWithDebInfo`), linked and ran.
Self-reported at runtime:

```
qt_runtime=6.11.2
qt_compiled=6.11.2
style=windows11
colorScheme=2 (0=Unknown,1=Light,2=Dark)
dpr=1.25
vst3_module_create_returned=null
vst3_module_error=LoadLibraryW failed for path nonexistent.vst3: The specified module could not be found.
foreign_hwnd_embedded=yes
```

Interpretation:

- `style=windows11` -- the native Windows 11 style is active, not an emulation.
- `colorScheme=2` -- dark mode detected from the system with no application code.
- `dpr=1.25` -- per-monitor DPI scaling is live.
- The `vst3_module_error` line is the *expected* failure for a nonexistent path, and proves
  `VST3::Hosting::Module::create` linked and executed -- i.e. the SDK hosting layer works.
- `foreign_hwnd_embedded=yes` -- the VST3 `IPlugView` embedding path works.

A screenshot confirmed native Windows 11 dark chrome with the correct system accent colour
applied to checkbox and slider, rendering crisply at 125% scaling, with nothing hand-styled.
The embedded foreign HWND region was blank in the capture, which is the expected behaviour
described in sec. 6.5.

**Traps sec. 6.3.1 through sec. 6.3.5 were each hit and resolved during this exercise**, which is why
they are documented as prerequisites rather than left to be discovered during implementation.

---

## 11. Open items

| # | Item | Owner | Blocks | State |
|---|---|---|---|---|
| 1 | Pin the VST3 SDK archive `URL_HASH` | implementation | first build | **Closed** 2026-08-21; pinned in sec. 6.3.2 |
| 2 | Confirm minimum OS floor (sec. 8.1) | project owner | APO stage | Open |
| 3 | Independently verify GFX vs modern slot behaviour (sec. 8.2) | project owner | APO stage | Open |
| 4 | Staged porting plan | project owner | -- | Open |

---

## 12. References

- [dechamps/APO -- notes on Windows Audio Processing Objects](https://github.com/dechamps/APO/blob/master/README.md)
- [Windows 11 APIs for Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects)
- [Implementing Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects)
- [Equalizer APO -- SFX/MFX on Windows 11 discussion](https://sourceforge.net/p/equalizerapo/discussion/general/thread/8ed33d9e1a/)
- [VST 3 now available under MIT license](https://www.steinberg.net/press/2025/vst-3-8/)
- [VST 3 licensing FAQ](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Licensing.html)
- [VST 3 hosting FAQ](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Hosting.html)
- [Qt High DPI documentation](https://doc.qt.io/qt-6/highdpi.html)
- [Dark mode on Windows 11 with Qt](https://www.qt.io/blog/dark-mode-on-windows-11-with-qt-6.5)
- [Qt window embedding example](https://doc.qt.io/qt-6/qtdoc-demos-windowembedding-example.html)
- [What's new in WPF for .NET 9 (Fluent theme)](https://learn.microsoft.com/en-us/dotnet/desktop/wpf/whats-new/net90)
- [Qt6 status in conda-forge](https://conda-forge.org/blog/2026/07/01/qt6-status-in-conda-forge/)
- [WiX Toolset release notes](https://docs.firegiant.com/wix/whatsnew/releasenotes/)
- [JUCE 8 licence](https://juce.com/legal/juce-8-licence/)
