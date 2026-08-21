# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

**Stage 1 (client IPC foundation) is implemented and verified.** What exists:

```
protocol/            header-only protocol v1 -- layout, names, planar addressing, header access
rt/                  RealtimeGuard + allocation/lock violation detector, fixed-capacity SPSC queue
ipc/                 BufferValet, ValetThread (promoted audio thread), ValetSupervisor, endpoints
tests/               27 Catch2 tests incl. the sec. 4.7 conformance harness (synthetic king)
tools/valet_probe/   console client -- attaches to the deployed APO, reports block stats
```

Not written yet: `engine/`, `scanner/`, `ui/`, `apo/`, `installer/`. The VST3 SDK is therefore
not wired into CMake yet, and open item sec. 11.1 (pin the SDK archive `URL_HASH`) still blocks the
first `engine/` build.

Build and test:

```
pixi run configure    # see the note in pixi.toml about CMAKE_GENERATOR_PLATFORM
pixi run build        # RelWithDebInfo -- the only usable configuration (sec. 6.4)
pixi run test         # ctest, 27 tests, ~2 s
pixi run probe        # attach to the real APO on the default render endpoint
```

Verified against the *deployed, unmodified* APO on the development machine: the client attaches
as a v1 valet and processes real blocks from `audiodg.exe` (48 kHz, 2 ch, 480 frames) with no
timeouts, no malformed headers and no reclaims. Object names, the planar layout and the endpoint
GUID form are therefore confirmed against the real producer, not just against the harness.

`design_doc.md` is the normative specification for this project. Read the relevant section
before writing code; it records decisions that were made empirically and should not be
re-litigated. Section references below (sec. n) point into it.

- **Predecessor:** `D:\automatl\audio-ipc` (`TomatlVst.sln`, last commit `1a1d3ea`). Not a
  submodule, not vendored -- a separate working copy on this machine. It is the reference
  implementation for protocol v1 (`TomatlAudioIpc/FastStream.h`, `AudioIpcApo/AudioIpcApo.cpp`)
  and for the SPSC queue pattern (`TomatlVst/spsc_queue.h`). Consult it; do not port it
  wholesale -- it is a C#/C++/CLI/VST2/WinForms stack that is being replaced entirely.

## What the system does

A Windows-only system-wide audio processor. A user-mode APO inside `audiodg.exe` intercepts
every audio block from the Windows audio engine and hands it, via shared memory, to a userspace
process that runs a VST3 plugin chain -- synchronously, on the audio engine's own clock, with
zero added latency.

Two halves, two roles in the IPC protocol:

- **King** -- the APO (producer, creates all shared objects). Lives in `audiodg.exe`.
- **Valet** -- the userspace client (consumer/processor). Opens existing objects.

**The client is rewritten first (sec. 1.3, sec. 7.3).** The existing unmodified APO stays deployed; the
new client attaches as a protocol v1 valet. All APO-side decisions (sec. 8) are deferred and do not
block client work.

## Hard rules

### 1. Protocol v1 is frozen (sec. 4)

Sec. 4 is a normative wire specification, not a description of intent. The new client must
interoperate with the *existing* APO binary. Deviations are defects, not improvements --
improvements go to sec. 9.1 (a future v2). Specifically:

- Object names, the 1 MiB mapping, and both events being **manual-reset** are fixed (sec. 4.2).
- The payload is **planar/de-interleaved**, shared in place: sample `s` of channel `c` at float
  index `c * (size / channelCount) + s`. `size` is a total sample count, not a frame count (sec. 4.3).
- Read `sampleRate` and `channelCount` from the shared header **on every block**. Never cache
  them across blocks -- the deployed APO has a `smartOpen` bug (`&&` where `||` was meant) that
  leaves a stale `sampleRate` after a sample-rate-only format change (sec. 3.7.3, sec. 4.5).
- If `valetId` at offset 0 stops matching this valet's own id, another client has taken over
  (`Stolen`) -- detach. Takeover is by design (sec. 4.1).
- Promote the valet thread exactly as the reference does: `SetThreadPriority(..., 15)`,
  `AvSetMmThreadCharacteristics(L"Pro Audio", ...)`, then
  `AvSetMmThreadPriority(..., AVRT_PRIORITY_CRITICAL)` (sec. 4.6).
- Dead legacy that is **not** part of v1 and must not be reintroduced: `MAIN_SIZE`/`MAIN_NAME`,
  `UTILITY_*`, `STREAM_INFO_MUTEX_NAME`, `SRV_WAIT`/`CLT_WAIT`, `SimpleMutex`,
  `ThreadSafeContainer` (sec. 4.2, sec. 7.4.4).

### 2. The audio thread is real-time safe, without exception (sec. 7.4)

This is a merge gate, not an aspiration. Read sec. 7.4 in full before touching `protocol/`, `ipc/`,
or anything the valet thread calls. Summary:

- **Forbidden on the audio thread:** any heap activity (`new`/`delete`, `std::vector`/`string`
  growth, `std::function` with non-inlinable capture, `shared_ptr` creation); any lock or
  condition variable; any I/O or logging; `LoadLibrary`/`GetProcAddress`/COM activation;
  throwing exceptions; unbounded loops; first-touch page faults (sec. 7.4.1).
- **The one sanctioned exception** is exhaustive: `SetEvent`/`ResetEvent`/`WaitForSingleObject`
  on the `KING` and `VALET` events, and reads/writes through the mapped view -- because that
  rendezvous *is* the protocol (sec. 7.4.4).
- Preallocate at stream open and touch every page. Control plane <-> audio thread communication
  goes through lock-free SPSC queues with bounded drain per block (sec. 7.4.2).
- **Plugin chain mutation never happens on the audio thread.** Build the chain on the control
  thread (`setupProcessing`, `setActive`, state restore, buffer allocation all happen there),
  publish with a single atomic pointer store, retire the old chain on the control thread. If
  what runs on the audio thread is more than a pointer swap, the work was not pushed far enough
  upstream -- that is a design defect (sec. 7.4.3).
- **Acceptance criterion (sec. 7.4.3):** user-initiated transitions *may* click -- that is acceptable
  and expected. Steady state must be **exactly zero** allocations and flat RSS. Both are
  testable; enforce them.
- Our VST3 host interface implementations are on this call stack too.
  `IComponentHandler::performEdit` can legitimately be called by a plugin *from its processing
  thread* -- it must enqueue lock-free and return (sec. 7.4.5).
- `RealtimeSanitizer` is unavailable under MSVC. The substitute is a debug-only violation
  detector: a thread-local "in RT section" flag set by an RAII guard, checked from global
  `operator new`/`operator delete` overrides and assert-only wrappers around locking
  primitives. Compiled into `RelWithDebInfo`, out of release (sec. 7.4.6).

### 3. Source and documentation files are ASCII-only (sec. 6.6)

Every tracked text file -- source, headers, CMake, TOML, JSON, Markdown -- contains only bytes
in 0x00-0x7F. Write `sec. 4.3` not the section sign, `--` not an em dash, `->` not an arrow,
`/` not a middle dot, plain `'` and `"` not curly quotes. Non-ASCII data that a *test* needs at
run time goes in as an escape sequence, never as a literal character.

This is enforced by `tests/source_hygiene_test.cpp`, which walks the tree on every `ctest` run
and reports file, line, column and byte. It is not a style preference: a section sign in a
Catch2 test name silently breaks the `ctest` name-to-filter round trip. The same class of
transport bug has already halved a doubled backslash in `kNamePrefix` here, producing an object
name that would never have matched the APO's -- caught only because MSVC happened to warn about
the resulting invalid escape sequence (C4129).

## Planned structure (sec. 7.1)

```
protocol/   header-only C++20 -- protocol v1, single source of truth (client, APO, tests)
ipc/        BufferValet, valet thread, thread promotion
engine/     VST3 host: module loading, plugin chain, state, processing graph
scanner/    separate executable -- out-of-process plugin probing (crash isolation)
ui/         Qt 6 Widgets shell, plugin rack, editor hosting, EQ/plot widgets
apo/        later stage -- the rewritten APO
tests/      Catch2: protocol conformance, engine, scanner
installer/  WiX v7
```

One userspace process hosts GUI + plugins + editors + valet thread (plugin editors need a UI
thread in the same process as their HWND, so splitting GUI from engine would buy nothing). One
short-lived scanner process per scan (sec. 7.2).

## Stack (sec. 5, sec. 6) -- decided, verified end to end

C++20 / MSVC v143 / Qt 6 Widgets (LGPLv3) / VST3 SDK 3.8.1 (MIT) linked directly.

The deciding constraint is that VST3 exposes plugin editors as `kPlatformTypeHWND` -- the host
must hand out a real child HWND. That, not aesthetics, is why Qt Widgets wins and why WinUI 3
and web UIs are disqualified (sec. 5.1, sec. 5.4). Embedding uses `QWindow::fromWinId()` ->
`QWidget::createWindowContainer()`.

Toolchain: **pixi** (`pixi.toml` + `pixi.lock` committed) providing `qt6-main`, `cmake`,
`ninja`, `vs2022_win-64`, `catch2`. CMake with `CMakePresets.json`, **Ninja Multi-Config**
generator, tests via Catch2 v3 + `ctest`. Third-party source deps via **`FetchContent` only** --
no submodules, no vcpkg, no Conan.

`vs2022_win-64` *activates* a local MSVC install (MSVC is not redistributable), so `cl.exe`
resolves inside `pixi run` with no `vcvars` and no Developer Command Prompt. Visual Studio 2022+
with the Desktop C++ workload and Windows SDK 10.0.26100+ remain machine prerequisites, as does
long-path support -- both `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1`
and `git config --system core.longpaths true` (sec. 6.2).

### Build configuration: `RelWithDebInfo`, always

conda-forge `qt6-main` ships **no debug libraries**, so a `Debug` configuration cannot link
(`/MDd` app vs `/MD` Qt -> `_ITERATOR_DEBUG_LEVEL` mismatch, `LNK2038`). `RelWithDebInfo` is
`/MD -Zi` with full PDBs and is fully debuggable -- it is the primary development configuration
and the one all verification in sec. 10 used (sec. 6.4).

### Build traps -- each was hit and resolved already (sec. 6.3)

Every one of these fails in a way that does not point at its cause. Do not rediscover them.

1. **MAX_PATH is the most severe.** The VST3 SDK has ~120-character internal paths. At a
   142-character source root, git fails with "Filename too long" and MSVC with
   `C1083: Cannot open compiler generated file: '': Invalid argument`. Keep source and build
   trees shallow. `GIT_CONFIG core.longpaths=true` inside `FetchContent_Declare` does **not**
   help -- it does not apply to submodule clones (tested and confirmed ineffective).
2. **Fetch the release archive, never `GIT_REPOSITORY`.** The git repo pulls seven
   relative-URL submodules totalling 501 MB and is the main MAX_PATH trigger. Use the pinned
   archive URL with `URL_HASH SHA256=...`; discover the current URL from the 302 redirect at
   `https://www.steinberg.net/vst3sdk`. There are no GitHub release assets.
3. **`SOURCE_SUBDIR vst3sdk` is mandatory** -- the archive root has no `CMakeLists.txt`. Without
   it, `FetchContent_MakeAvailable` silently skips `add_subdirectory`, `sdk_hosting` never
   exists, and the failure surfaces as a *missing header* error.
4. **Add `public.sdk/source/vst/hosting/module_win32.cpp` to your own target's sources.**
   `smtg_create_public_sdk_hosting_target()` omits the platform module loader by design.
   Symptom: one `LNK2019` on `VST3::Hosting::Module::create` and no other diagnostic.
5. **The SDK's CMake is not a well-behaved dependency.** `SMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF`
   does not gate the sample subdirectories, so consuming via `add_subdirectory` always builds
   `validator`, `editorhost`, `inspectorapp` and `moduleinfotool` -- a one-file test app produced
   66 targets. Tolerable initially; sec. 9.6 is to replace it with an in-house static library over
   the ~20 translation units `sdk_hosting` actually lists.

Also: `QWidget::grab()` renders Qt's own backing store, so an embedded foreign HWND captures
blank. Use `PrintWindow`/`BitBlt` if a plugin editor ever needs capturing (sec. 6.5).

## Testing

The protocol conformance harness is a **first-class deliverable**, replacing the predecessor's
manual interactive `DebugStream` (sec. 4.7). It needs a synthetic king (drives blocks at
configurable rate/format, asserts round-trip latency bounds) and a synthetic valet (for
exercising a rewritten APO later), covering: attach/detach, `Stolen` takeover, king-side timeout
eviction, mid-stream format change, planar round-trip fidelity, and the stale-`sampleRate` case.

Plus the sec. 7.4.3 soak test: flat audio-thread allocation count (exactly zero) and flat RSS over a
long idle run.

## Known defects in the deployed system

Inherited, and the client must tolerate rather than fix them -- fixing requires protocol v2:

- The APO blocks the audio engine's real-time thread for up to **1000 ms** waiting on the valet.
  A slow or stalled client causes a system-wide audio dropout. This is v1's known weak point and
  the reason sec. 9.1 exists (sec. 3.7.1).
- All shared objects use a **null DACL** -- any process on the machine can open them, steal the
  stream, inject audio, or wedge `audiodg.exe` (sec. 3.7.2, sec. 9.2).
- Registry ownership of endpoint keys is taken and never restored (sec. 3.7.5, sec. 9.4).

## Open items (sec. 11)

1. Pin the VST3 SDK archive `URL_HASH` -- blocks the first build.
2. Minimum OS floor (sec. 8.1, leaning Windows 11 only) -- project owner, blocks APO stage.
3. Independently verify GFX vs modern registration slots (sec. 8.2, leaning GFX `,2` only, since the
   modern SFX/MFX/EFX slots are reportedly broken for third-party APOs on Windows 11 24H2) --
   project owner, blocks APO stage.
4. Staged porting plan -- project owner.
