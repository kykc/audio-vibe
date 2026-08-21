# System-wide audio processing utility — rewrite design document

**Status:** accepted (stack + toolchain). Implementation not started.
**Date:** 2026-08-21
**Predecessor:** `D:\automatl\audio-ipc` (`TomatlVst.sln`), last commit `1a1d3ea`.

---

## 1. Purpose and scope

The predecessor project provides system-wide audio compensation/processing on Windows by
inserting a user-mode Audio Processing Object (APO) into the Windows audio engine and
forwarding every audio block to a userspace host process that runs a chain of audio plugins.

This document covers:

- **Stage 0** — analysis of the existing solution and verification of the stated motivations.
- **Stage 1** — technology stack decision.
- **Stage 1.5** — toolchain, dependency and packaging decisions.

It also freezes the **v1 IPC protocol** as a normative specification, so that the userspace
client and the APO can be rewritten independently and sequentially.

Out of scope for this document: the staged porting plan, UI design, and DSP feature set.

### 1.1 Rewrite motivations

Carried over from the predecessor, verified in §3:

1. Originally written against .NET 2 and increasingly painful to maintain.
2. Mixture of managed C# and C++/CLI.
3. Written as a VST2 host; VST2 is out of support and current Windows plugins ship VST3.
4. APO registration nomenclature changed since Windows 7; the project still uses the
   deprecated `FX` registry entries.
5. UI/UX is limited by Windows Forms.

### 1.2 Guiding constraints

- **Windows-exclusive.** APO and VST3 both make this unavoidable. No cross-platform budget.
- **Preserve the APO ↔ userspace data exchange protocol.** This is what allows the rewrite to
  proceed in independent stages. See §4.
- **No web UI.** Multi-process browser runtimes with hundreds of MiB of RSS are a poor fit for a
  background audio utility, and wiring native VST3 plugin editors into a web view is worse.
  Stated as a preference, treated as a decision (see §5.4).
- **Modern UX expectations are mandatory:** per-monitor DPI (including mixed-DPI multi-monitor),
  dark theme, accessible native controls.

### 1.3 Rewrite order

**The userspace client is rewritten first.** The existing APO stays deployed and unmodified;
the new client speaks protocol v1 as a `BufferValet`. This gives a working system at the end of
the first stage with no changes to the kernel-adjacent, hard-to-debug half.

Consequences: the APO-side decisions in §8 are deferred and do **not** block client work.

---

## 2. Existing solution inventory

12 projects, ~13,600 lines of hand-written source (excluding vendored dependencies).

| Project | Language | Role |
|---|---|---|
| `AudioIpcApo` | C++ (no CLR) | The APO. Registers in the GFX slot, aggregates a child APO, performs a blocking rendezvous with userspace |
| `TomatlAudioIpc` | C++ headers | IPC protocol: `AudioIpc.h` (Win32 primitives), `FastStream.h` (`BufferKing`/`BufferValet`) |
| `TomatlVst` | C++ | VST2.4 host, `VstStream` worker thread, `spsc_queue` for control requests |
| `TomatlVstInterop` | **C++/CLI** | 583-line managed/native marshalling bridge |
| `DeRack` | C# WinForms | Main GUI — one tab per endpoint, plugin rack |
| `Tomatl` | C# | Plot/PEQ custom controls, MMDevice enumeration, APO registration, package model, JSON storage |
| `Config` | C# WinForms | Configurator (endpoint enable/disable) |
| `Cli` | C# | `write-mmd`, `build-package` |
| `Installer` | C# WinForms | Homebrew installer |
| `Tomatl.Local` | C# | Branding strings |
| `ParametricEqVst` | C++/CLI | In-house parametric EQ as a VST2 plugin |
| `DebugStream` | C++ | Loopback test harness — a fake `BufferKing` that measures round-trip latency |
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
liability (§9.1).

### 2.2 Registration mechanism

`Tomatl.MMDevice.DeviceAction.performApoExchange` writes, per render endpoint under
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{guid}\FxProperties`:

- `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2` (GFX / post-mix effect CLSID)
  := `{B6A6A861-A99F-4F00-B636-657F38F353E9}` (this project's APO)
- `OriginalGfxApo` := the previous value of the above (custom, non-standard value name)

To do this it takes ownership of the endpoint key and replaces its DACL with one granting
`BUILTIN\Administrators` full control. The original owner is never restored — the restore code
exists but is commented out.

At runtime `AudioIpcApo::Initialize` reads `OriginalGfxApo`, and if it names a valid CLSID,
`CoCreateInstance`s it and chains it ahead of itself. **This feature is dropped in the rewrite**
(§8.3).

---

## 3. Fact verification

### 3.1 ".NET 2" — outdated description, conclusion stands

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

### 3.2 Managed + C++/CLI fusion — confirmed

`TomatlVstInterop` (583 lines, `CLRSupport=true`) and `ParametricEqVst` (`CLRSupport=true`).
The project's own initial commit message describes it as "insane native/managed fusion".

### 3.3 VST2 — confirmed, and licensing has improved dramatically

The host builds against a vendored VST 2.4 SDK (`Dependencies/VstSdk24`,
`pluginterfaces/vst2.x/aeffect.h` etc.). Steinberg terminated VST2 licensing in 2018 and no
longer distributes the SDK.

**New, material fact:** the VST3 SDK was **relicensed to MIT** with version 3.8 (announced
November 2025). There is no longer a GPLv3-or-proprietary dual license, no signed Steinberg
developer agreement, and no source-disclosure obligation. This removes the largest historical
obstacle to a VST3 rewrite.

### 3.4 APO nomenclature — confirmed, but migration is not a straightforward win

The modern and legacy property IDs under `FxProperties`, all with GUID
`{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}`:

| Slot | Property ID | Status |
|---|---|---|
| SFX (stream effect) | `,5` | modern |
| MFX (mode effect) | `,6` | modern |
| EFX (endpoint effect) | `,7` | modern |
| LFX (local — legacy name for SFX) | `,1` | deprecated |
| GFX (global — legacy name for MFX) | `,2` | deprecated — **currently used by this project** |

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
See §8.2.

### 3.5 Windows Forms UX ceiling — confirmed

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
   a full second. See §9.1.
2. **Null DACL on every shared object.** `SetSecurityDescriptorDacl(sd, TRUE, NULL, FALSE)` is
   used for the file mapping, both events and the mutex. Any process on the machine can open
   them, take over the stream (the `valetId` steal is by design), inject audio, or wedge
   `audiodg.exe`. See §9.2.
3. **Logic bug in `BufferKing::smartOpen`:**
   `sampleRate != _sampleRate && channelCount != _channelCount` — the `&&` should be `||`.
   A sample-rate-only change (44.1 → 48 kHz at constant channel count) leaves a stale
   `sampleRate` in the shared header.
4. **Will not compile at modern language levels.** `std::uniform_int` and `std::uniform_real`
   (used in `BufferValet::generateClientId` and `DebugStream`) were removed in C++17.
5. **Registry ownership is taken and never returned** (§2.2).
6. **The vendored `Dependencies/baseaudioprocessingobject.h` is no longer needed.** Verified on
   this machine: Windows SDK **10.0.26100** ships
   `Include/10.0.26100.0/um/baseaudioprocessingobject.h` and
   `Lib/10.0.26100.0/um/{x64,arm64,x86}/audiobaseprocessingobject.lib`. No WDK is required,
   and **ARM64 is available at no additional cost**.
7. **Plugin probing happens in-process**, so a malformed plugin can hang or crash the GUI.
8. **No automated tests.** `DebugStream` implements the right idea (a synthetic `BufferKing`
   measuring round-trip latency) but is a manual, interactive console program.

---

## 4. Protocol v1 — normative specification

This section is **frozen**. The rewritten client must interoperate with the *existing*
unmodified APO, and a future rewritten APO must interoperate with the rewritten client.
Deviations are defects. Improvements are deferred to §9.1.

Derived from `TomatlAudioIpc/FastStream.h` and `AudioIpcApo/AudioIpcApo.cpp` at commit `1a1d3ea`.

### 4.1 Roles

- **King** — the APO, inside `audiodg.exe`. Creates all objects. Producer of unprocessed audio.
- **Valet** — the userspace client. Opens existing objects. Consumer/processor.

Exactly one valet may be attached to a given endpoint at a time. A newly arriving valet
*displaces* the incumbent; this is intentional.

### 4.2 Named objects

One set per render endpoint. Let `GUID` be the endpoint GUID as returned by
`PKEY_AudioEndpoint_GUID`, used verbatim — including braces — for object names. (The APO
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
- Objects are created with a null DACL (§9.2).
- The APO creates the mapping with `CreateFileMapping(INVALID_HANDLE_VALUE, ...)`; the valet
  also calls `CreateFileMapping` with the same name and size, which attaches to the existing
  mapping.

### 4.3 Shared memory layout

Little-endian, packed, no padding. `ULONG` and `int` are 4 bytes.

| Offset | Type | Field | Written by |
|---|---|---|---|
| 0 | `ULONG` | `valetId` — 0 means "no client attached" | valet (claim), king (eviction) |
| 4 | `ULONG` | `sampleRate` | king |
| 8 | `ULONG` | `channelCount` | king |
| 12 | `int` | `size` — **total** sample count, i.e. `frames * channelCount` | king |
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
   If either does not exist, the endpoint is not active — fail and retry later.
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

1. `WaitForSingleObject(VALET, timeout)` — the reference implementation passes `INFINITE`.
2. Read `valetId` at offset 0. **If it does not match this valet's own id, the stream has been
   taken over by another client (`Stolen`) — the valet must detach.**
3. Read `sampleRate`, `channelCount`, `size`. Compute `blockSize = size / channelCount`.
4. Process the planar payload in place.
5. `ResetEvent(VALET)`; `SetEvent(KING)`.
   To detach cleanly, write `0` to `valetId` before this step.

### 4.5 Lifecycle

The king calls `smartOpen(sampleRate, channelCount)` from `LockForProcess` and `smartClose()`
from its destructor. On open it sets `KING` and clears `VALET`; if it created the mapping fresh
it zeroes `valetId`. On close it clears both events and releases all handles — the valet then
sees its waits fail and must re-attach.

Note the `smartOpen` bug in §3.7.3: a sample-rate-only format change does not trigger a
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

### 4.7 Conformance testing

A protocol conformance harness is a first-class deliverable, replacing the manual
`DebugStream`:

- a synthetic king that drives blocks at a configurable rate/format and asserts round-trip
  latency bounds;
- a synthetic valet, for exercising a rewritten APO later;
- tests for: attach/detach, takeover (`Stolen`), king-side timeout eviction, format change
  mid-stream, planar round-trip fidelity, and the §4.5 stale-`sampleRate` case.

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

**C++20 · single userspace process · Qt 6 Widgets · VST3 SDK linked directly.**

| Layer | Choice | Rationale |
|---|---|---|
| Userspace host | C++20, MSVC | One language for the whole system, including the future APO. No marshalling layer. |
| GUI | **Qt 6 Widgets** (LGPLv3) | Per-Monitor-v2 aware by default; native `windows11` style with real dark mode; every widget can yield an HWND; `QPainter` is a direct port target for the existing `Plot`/`PeqControl` custom drawing. |
| Plugin editor hosting | `QWindow::fromWinId()` + `QWidget::createWindowContainer()` | The standard Qt technique for embedding a foreign child HWND. Verified working (§10). |
| Plugin format | **VST3 SDK 3.8.1**, MIT | Use `public.sdk/source/vst/hosting` (`module.h`, `PlugProvider`, host classes). |
| Plugin scanning | **Separate short-lived probe process** | Fixes §3.7.7 — a malformed plugin can no longer take down the GUI. |
| Audio thread | Own valet thread + `AvSetMmThreadCharacteristics("Pro Audio")` | Per §4.6. No audio framework needed. |
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

Rejected because it reintroduces a language boundary and a second build system — precisely
motivation §1.1.2 — and adds a .NET runtime deployment.

### 5.4 Rejected alternatives

| Option | Reason for rejection |
|---|---|
| **WinUI 3 / Windows App SDK** | Controls are windowless composition surfaces; there is no supported `HwndHost` equivalent. Hosting a plugin's child HWND leads to airspace defects. Disqualified by §5.1. |
| **Web UI** (Electron/Tauri/webview) | Multi-process RAM footprint is inappropriate for a background audio utility, and the plugin-editor HWND problem is unsolved — a native side-window would still be required, paying both costs. Per §1.2. |
| **Dear ImGui** | Acceptable for a debug overlay; wrong for accessibility, IME, and per-monitor text quality. |
| **Rust** | VST3 hosting bindings are thin; a COM APO deriving from `CBaseAudioProcessingObject` is C++-shaped work. |
| **JUCE 8** | Genuinely attractive — hosting, out-of-process scanning and plugin state for free. Rejected because its non-native look fights §1.2, and it imposes AGPLv3-or-commercial where the VST3 SDK is now MIT. **Reconsider only if VST3 hosting proves harder than projected.** |

### 5.5 VST2

**Dropped.** If a VST2-only plugin is ever genuinely needed, a third-party VST2→VST3 wrapper
can be used, or a narrow in-house wrapper written against the vendored 2.4 SDK in the
predecessor repository. Not redistributable, so not shipped.

---

## 6. Toolchain and dependencies

### 6.1 Decision

| Concern | Choice |
|---|---|
| Environment / tools | **pixi** — `pixi.toml` and `pixi.lock` committed to the repository |
| pixi packages | `qt6-main`, `cmake`, `ninja`, `vs2022_win-64`, `catch2` |
| Compiler | Local MSVC v143, activated automatically by `vs2022_win-64` |
| Build system | CMake (4.x from pixi) with `CMakePresets.json` |
| Generator | **Ninja Multi-Config** |
| Primary configuration | **`RelWithDebInfo`** (see §6.4) |
| Third-party source deps | **`FetchContent` only.** No git submodules, no vcpkg, no Conan |
| VST3 SDK | `FetchContent` from the pinned official release archive + `URL_HASH` |
| Windows SDK | 10.0.26100 (already installed) |
| Architectures | x64 primary, ARM64 supported (§3.7.6) |
| Installer | **WiX v7** (.NET tool / MSBuild SDK) |
| Qt deployment | `windeployqt6` / `qt_generate_deploy_app_script` |
| Tests | Catch2 v3 + `ctest` |
| Hygiene | `clang-format`, `clang-tidy`, `/W4 /permissive-`, `/analyze` on the APO |

Rationale for pixi over vcpkg/Conan: the whole toolchain — compiler activation, Qt, CMake,
Ninja, test framework — comes from one lockfile, reproducibly, with no source builds. Verified
end to end in §10. `vcpkg`'s `qtbase` port is a multi-hour source build; the official Qt
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
4. **Long path support enabled** — see §6.3.1. Both of the following:
   - `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1`
   - `git config --system core.longpaths true`

### 6.3 Known build traps

These were all discovered empirically (§10) and each fails in a way that does not obviously
point at its cause.

#### 6.3.1 MAX_PATH — the most severe

The VST3 SDK contains paths such as
`public.sdk/samples/vst/note_expression_synth_auv3/iOS/Resources/Assets.xcassets/AppIcon.appiconset/apple-icon-120x120.png`
(~120 characters), and the SDK's own CMake generates object paths like
`validator.dir/RelWithDebInfo/__/__/__/source/vst/...`.

Observed failures at a 142-character source root:

- `git`: `error: unable to create file ...: Filename too long`, then
  `fatal: Unable to checkout ... in submodule path 'public.sdk'`
- MSVC: `fatal error C1083: Cannot open compiler generated file: '': Invalid argument`

The identical tree at a ~30-character root builds cleanly.

**Mitigations:** keep the source and build trees shallow; enable long path support per §6.2.4.
Note that `GIT_CONFIG core.longpaths=true` inside `FetchContent_Declare` does **not** help — it
applies only to the top-level clone, not to submodule clones. This was tested and confirmed
ineffective.

#### 6.3.2 Use the release archive, not the git repository

`GIT_REPOSITORY` pulls seven submodules declared with *relative* URLs, totalling **501 MB** —
including `vstgui4`, `doc` and `tutorials`, none of which a host needs. It is also the primary
trigger for §6.3.1. Restricting `GIT_SUBMODULES` does not help, because `public.sdk` itself
contains the offending long paths.

The official release archive is 246 MB extracted, contains no submodules, and can be pinned by
content hash:

```cmake
FetchContent_Declare(vst3sdk
  URL https://download.steinberg.net/sdk_downloads/vst-sdk_3.8.1_build-84_2026-08-11.zip
  URL_HASH SHA256=<pin this>
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR vst3sdk
)
```

`https://www.steinberg.net/vst3sdk` redirects (302) to that exact URL, which is how to discover
the current one. There are no GitHub release assets for this repository.

#### 6.3.3 `SOURCE_SUBDIR` is mandatory

The archive root contains `vst3sdk/` and `VST3_Project_Generator/` but **no `CMakeLists.txt`**.
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
`add_subdirectory(public.sdk/samples/vst-utilities)` are **unconditional** —
`SMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF` does not gate them. Consuming the SDK via
`add_subdirectory` therefore always builds `validator`, `editorhost`, `inspectorapp` and
`moduleinfotool`. A single-file test application produced 66 targets.

This is tolerable as a one-time cost. **Preferred longer-term approach:** bypass the SDK's
CMake and declare our own static library over the ~20 translation units that `sdk_hosting`
actually lists (`public.sdk/source/vst/hosting/*.cpp`, `utility/stringconvert.cpp`,
`vstinitiids.cpp`) plus `pluginterfaces` and `base`. For a host this is a small, well-understood
surface and gives full control over flags and build time. Revisit once the client's shape
settles.

### 6.4 Qt is Release-only

The conda-forge `qt6-main` package ships **no debug libraries** — verified: no `Qt6Cored.dll`,
no `Qt6Widgetsd.dll`. Consequently a `Debug` configuration is unavailable: a `/MDd`
application cannot link `/MD` Qt (`_ITERATOR_DEBUG_LEVEL` mismatch, `LNK2038`).

**`RelWithDebInfo` is the primary development configuration** — `/MD -Zi`, full PDBs, fully
debuggable. This is the configuration used for all verification in §10.

If a genuine debug Qt is ever required, that is the one circumstance that would justify moving
Qt to the official installer or a vcpkg source build.

### 6.5 `QWidget::grab()` does not capture embedded plugin editors

`grab()` renders Qt's own backing store; a foreign child HWND is composited by Windows and
appears blank. Irrelevant for normal operation, but it means editor thumbnails or automated
visual tests over plugin GUIs require `PrintWindow`/`BitBlt` instead.

---

## 7. Target architecture

### 7.1 Components

```
audio-ipc2/
  protocol/        header-only C++20 library — protocol v1 (§4), the single source of truth
  ipc/             BufferValet implementation, valet thread, thread promotion (§4.6)
  engine/          VST3 host: module loading, plugin chain, state, processing graph
  scanner/         separate executable — out-of-process plugin probing
  ui/              Qt 6 Widgets shell, plugin rack, editor hosting, EQ/plot widgets
  apo/             (later stage) the rewritten APO
  tests/           Catch2: protocol conformance (§4.7), engine, scanner
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

---

## 8. Deferred decisions

These are all APO-side and do not block client work (§1.3, §7.3).

### 8.1 Minimum OS version

**Leaning: Windows 11 only.** Windows 11 (build 22000+) unlocks the CAPX frameworks —
`IAudioSystemEffects3`, the settings property store, first-party APO logging, the real-time
work queue — and would let the APO stop hand-rolling registry access for its settings.
Supporting Windows 10 means retaining the `APOInitSystemEffects2` path indefinitely.

To be decided when APO work begins.

### 8.2 Registration slot policy

**Leaning: GFX (`,2`) only**, on the §3.4 evidence that the modern slots are currently
unreliable for third-party APOs on 24H2. Requires independent verification by the project
owner before being fixed.

Whatever is chosen must be **either/or, never both** (§3.4, finding 1), and should be
accompanied by a post-registration verification step that confirms the APO is actually being
loaded.

### 8.3 Child APO aggregation — dropped

The predecessor's ability to chain the OEM's original GFX APO (§2.2) is **removed**. It was
never used in practice and the predecessor's own commit history describes it as troublesome
(`47f449a`, "APO aggregation (causes trouble, but works somehow)").

---

## 9. Future improvements

Deliberately deferred to keep protocol v1 frozen (§1.2, §4). Recorded here so they are not
lost.

### 9.1 Protocol v2: bounded real-time blocking

**The single most important improvement.** Protocol v1 lets a stalled userspace process block
the audio engine's real-time thread for up to 1000 ms (§3.7.1), which manifests as a
system-wide audio dropout.

A v2 should:

- add a **version/capability word** to the shared header so v1 and v2 peers can negotiate,
  keeping v1 as the fallback;
- reduce the king-side timeout to a **fraction of the audio period** (single-digit
  milliseconds) with immediate, glitch-free bypass on expiry;
- consider decoupling entirely — a lock-free ring buffer with an explicit, declared latency
  budget — trading v1's zero added latency for the audio engine no longer being hostage to a
  userspace process. This is a real trade-off, not a free win, and deserves its own decision.

### 9.2 Tighten shared object security

Replace the null DACL (§3.7.2) with an explicit one granting access only to the audio service
identity and `BUILTIN\Administrators`. Requires coordinated change on both sides, hence v2.

### 9.3 Migrate to the modern registration slots

Revisit §3.4 once Microsoft fixes third-party SFX/MFX/EFX registration on current Windows 11
builds. Until then GFX is the pragmatic choice.

### 9.4 Restore registry ownership

The installer should restore the original owner and DACL of the endpoint key on uninstall
(§3.7.5).

### 9.5 Adopt CAPX

Contingent on §8.1. Would replace hand-rolled registry settings access, add structured
logging from inside `audiodg.exe` (a significant debuggability win), and provide a real-time
work queue.

### 9.6 Replace SDK CMake consumption with a minimal in-house target

Per §6.3.5.

---

## 10. Verification record

All of the following was executed on the development machine on 2026-08-21 and passed. This
is the evidence base for §5 and §6; it is recorded so the decisions can be re-examined rather
than re-litigated.

**Machine:** Windows 11 build 26200 (25H2) · VS 2022 Enterprise, MSVC 14.44.35207 ·
Windows SDK 10.0.26100 · pixi 0.76.2.

**Environment.** `pixi add qt6-main cmake ninja vs2022_win-64` resolved Qt **6.11.2** win-64
with complete CMake config packages, `moc` / `uic` / `rcc`, `windeployqt6`, and
`plugins/styles/qmodernwindowsstyle.dll` (the genuine Windows 11 style), plus CMake 4.4.2 and
Ninja 1.13.2. `vs2022_win-64` activated the local MSVC automatically inside `pixi run` —
`cl.exe` 19.44 resolved from the VS 2022 Enterprise install with no `vcvars` invocation and no
Developer Command Prompt.

**Integration probe.** A Qt Widgets application plus the VST3 SDK 3.8.1 obtained via
`FetchContent` (archive URL, `SOURCE_SUBDIR`), linking `sdk_hosting` and compiling
`module_win32.cpp`, with a foreign child HWND embedded via `QWindow::fromWinId()` →
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

- `style=windows11` — the native Windows 11 style is active, not an emulation.
- `colorScheme=2` — dark mode detected from the system with no application code.
- `dpr=1.25` — per-monitor DPI scaling is live.
- The `vst3_module_error` line is the *expected* failure for a nonexistent path, and proves
  `VST3::Hosting::Module::create` linked and executed — i.e. the SDK hosting layer works.
- `foreign_hwnd_embedded=yes` — the VST3 `IPlugView` embedding path works.

A screenshot confirmed native Windows 11 dark chrome with the correct system accent colour
applied to checkbox and slider, rendering crisply at 125% scaling, with nothing hand-styled.
The embedded foreign HWND region was blank in the capture, which is the expected behaviour
described in §6.5.

**Traps §6.3.1 through §6.3.5 were each hit and resolved during this exercise**, which is why
they are documented as prerequisites rather than left to be discovered during implementation.

---

## 11. Open items

| # | Item | Owner | Blocks |
|---|---|---|---|
| 1 | Pin the VST3 SDK archive `URL_HASH` | implementation | first build |
| 2 | Confirm minimum OS floor (§8.1) | project owner | APO stage |
| 3 | Independently verify GFX vs modern slot behaviour (§8.2) | project owner | APO stage |
| 4 | Staged porting plan | project owner | — |

---

## 12. References

- [dechamps/APO — notes on Windows Audio Processing Objects](https://github.com/dechamps/APO/blob/master/README.md)
- [Windows 11 APIs for Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects)
- [Implementing Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects)
- [Equalizer APO — SFX/MFX on Windows 11 discussion](https://sourceforge.net/p/equalizerapo/discussion/general/thread/8ed33d9e1a/)
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
