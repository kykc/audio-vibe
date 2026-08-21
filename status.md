# Project status

**Updated:** 2026-08-21, after the Engine milestone plus the first real third-party plugin
(ZL Equalizer 2), which found and fixed a side-chain bus defect.

**Purpose of this file.** To let a work session be thrown away and a fresh one started without
losing the plot. It records *where we stand* -- what is built, what is proven, what is next, and
which decisions were taken during implementation that the design document does not cover. It is
the one document here that is expected to change constantly.

Division of labour between the three documents, so nothing is written twice:

| Document | Contains | Changes |
|---|---|---|
| `design_doc.md` | The normative specification. What to build and why. Protocol v1 is frozen there. | Rarely, deliberately |
| `AGENTS.md` | Hard rules and working instructions for whoever (or whatever) edits this repo | Rarely |
| `status.md` (this file) | Current position, next actions, live blockers, implementation notes | Every session |

If this file and `design_doc.md` disagree about intent, `design_doc.md` wins and this file is
stale. If this file and the code disagree about fact, the code wins and this file is stale --
say so and fix it.

---

## 1. Orientation in sixty seconds

The system is a Windows-only system-wide audio processor: a user-mode APO inside `audiodg.exe`
hands every audio block, through shared memory, to a userspace process that runs a VST3 plugin
chain synchronously on the audio engine's clock. The APO is the **king** (producer), the
userspace client is the **valet** (consumer). See `AGENTS.md` for the full summary.

**The client is being rewritten first (sec. 1.3, sec. 7.3).** The old APO stays deployed and
untouched; the new client attaches to it as a protocol v1 valet. Every APO-side decision is
deferred and blocks nothing.

**Where we are:** the IPC foundation and a first VST3 host are both finished and verified against
the real deployed APO. A real VST3 plugin chain now runs inside the `audiodg.exe` block loop.
Nothing of the plugin scanner or the UI exists yet.

Prove the tree is healthy in one command:

```
pixi run test          # expect: 100% tests passed out of 39, ~3 s
```

Then, on a machine with the old APO installed, prove interop against real hardware:

```
pixi run probe                              # pass-through
valet_probe --inspect --plugin <path.vst3>  # load and prepare only; never attaches
valet_probe --plugin <path.vst3> --seconds 30
```

Expect a line per second reading `blocks N (+~101/s) timeouts 0 malformed 0 reclaims 0
48000 Hz x2 ch, 480 frames`, an `Audio-thread allocations/frees/locks` block reading zero,
and with `--plugin`, a `[chain] built for 48000 Hz x2 ch` line plus a `Chain blocks` total.

**Audio has to be playing on the endpoint.** `audiodg.exe` only creates the shared objects
while a stream is active, so with nothing playing the run reports `Attach cycles: 0` and
zero blocks. That is not a client bug, and it is not evidence the APO is missing either --
check playback first.

**Everything must run through `pixi run`.** Catch2 is a conda-forge shared library and its DLL is
only on `PATH` inside the pixi environment. A bare `aip_tests.exe` exits with a bare status code
and no output, which looks like a crash and is not one.

**The first build downloads 124 MB** (the VST3 SDK archive) and compiles five SDK libraries.
After that it is incremental.

---

## 2. Milestone map

Note on numbering: `design_doc.md` sec. 1 numbers its own *document* stages -- Stage 0 analysis,
Stage 1 stack decision, Stage 1.5 toolchain -- and all three are complete. Those are decisions,
not code. The milestones below are the *implementation* track and are deliberately named after
components rather than numbered, so the two schemes cannot be confused.

| Milestone | Scope | State |
|---|---|---|
| Design | Analysis, stack, toolchain, protocol v1 frozen | Done: `design_doc.md` sec. 2-6, and sec. 4 is frozen |
| IPC foundation | `protocol/`, `rt/`, `ipc/`, conformance harness, probe tool | Done and verified |
| **Engine** | **`engine/` -- VST3 host: module loading, plugin chain, preallocated process data, atomic publication** | **Done for a single linear chain; verified against the real APO** |
| Scanner | `scanner/` -- out-of-process plugin probing | Not started. Next up; see section 5 |
| UI | `ui/` -- Qt 6 Widgets shell, plugin rack, editor hosting | Not started |
| Installer | `installer/` -- WiX v7 | Not started |
| APO rewrite | `apo/` -- the rewritten APO | Deferred by design (sec. 7.3, sec. 8) |

"Done for a single linear chain" is deliberate: the engine builds and runs an ordered list of
plugins for one format, and rebuilds when the format changes. What it does *not* yet do is listed
in section 5.

---

## 3. What exists

```
protocol/   header-only, no Windows headers. layout.h (names, 16-byte header, limits),
            planar.h (PlanarView, interleave conversion, header validation),
            header_access.h (tear-free field access via std::atomic_ref)
rt/         realtime_guard.h (RealtimeGuard + violation counters), mutex.h (assert-only
            lock wrapper), spsc_queue.h (fixed-capacity lock-free ring),
            src/alloc_hooks.cpp (global operator new/delete replacements, OBJECT library)
ipc/        buffer_valet (attach/acquire/release/reclaim), valet_thread (the promoted audio
            thread and its loop), valet_supervisor (control thread: retry, re-attach, policy),
            manual_event, shared_mapping, null_dacl, thread_priority, endpoints (MMDevice)
engine/     audio_thread.h (which-thread-am-I marker), plugin_module (load a .vst3, enumerate
            audio-effect classes), plugin_instance (one prepared plugin + its HostProcessData),
            component_handler (IComponentHandler, lock-free from the audio thread),
            plugin_chain (ordered plugins + ping-pong scratch banks), chain_processor (the
            BlockProcessor: atomic pointer, format check, epoch-based retirement),
            engine (control-thread facade: load, rebuild, publish, service)
cmake/      vst3sdk.cmake -- the pinned SDK, and every workaround sec. 6.3 calls for
tests/      39 Catch2 tests. harness/synthetic_king (the sec. 4.7 producer), harness/
            test_processors, harness/wait_for, fixtures/aip_test_plugin (a real VST3 plugin
            built from the SDK). conformance, engine, planar, protocol_layout,
            realtime_safety, source_hygiene, spsc_queue
tools/      valet_probe -- console client against the real APO, with --plugin and --inspect
            (--inspect loads, instantiates and prepares plugins and reports bus layout,
            parameter count, latency and split-vs-single, without attaching to anything;
            it is the shape `scanner/` needs, one process further out)
```

Thread split, which is the load-bearing structural decision:

- **Control thread** (`ValetSupervisor`, `engine::Engine`): opens handles, generates the valet id,
  claims the stream, faults in every page, loads modules, activates plugins, builds and publishes
  chains, destroys retired ones, drains plugin callbacks. Allowed to allocate, log and lock.
- **Audio thread** (`ValetThread` -> `engine::ChainProcessor`): the sec. 4.4 rendezvous, one
  atomic pointer read, a geometry comparison, and `IAudioProcessor::process` per plugin. Nothing
  else. Real-time safe under sec. 7.4.1.

How a chain gets published, end to end (sec. 7.4.3):

1. `Engine::appendPlugin(path)` loads the module (cached by path) and records the class id.
   Nothing is instantiated yet.
2. The audio thread records the geometry of every block it sees into one packed atomic, whether
   or not a chain is running. That is the *only* place a format comes from -- protocol v1
   announces it nowhere else (sec. 4.5).
3. `Engine::serviceFormatChange` notices a geometry it has not built for, instantiates and
   prepares every plugin, and constructs a `PluginChain` with its scratch banks page-touched.
4. `ChainProcessor::publish` stores the new pointer, waits for the audio thread to leave the old
   chain (epoch counter, not a guessed grace period), and destroys it on the control thread.

---

## 4. What has actually been proven, and what has not

Proven, and re-checkable by running the suite:

- Protocol v1 conformance against the synthetic king across the whole sec. 4.7 list: attach and
  detach, takeover (`Stolen`), king-side timeout eviction followed by reclaim, format change
  mid-stream, planar round-trip fidelity, and the stale-`sampleRate` case of sec. 4.5.
- Malformed headers (zero channel count, negative size, size past the mapping, size not
  divisible by channel count) are rejected without faulting, and the rendezvous still completes
  so the king is not stalled for its full 1000 ms.
- 2000 consecutive blocks stay in lockstep -- the two-event handshake carries no sequence
  number, so a single missed `ResetEvent` would desynchronise it.
- The sec. 7.4.3 acceptance criterion, twice: **exactly zero** audio-thread allocations, frees
  and lock acquisitions over 20,000 blocks with an inert processor and flat resident set, *and*
  over 7,000 blocks with a real VST3 plugin in the chain issuing eight `performEdit` callbacks
  per block. The detector has its own self-tests, so a zero reading means the instrument is live
  rather than absent.
- Overflowing the `performEdit` ring costs nothing on the audio thread: with the control thread
  deliberately starved for 2,000 blocks, edits are dropped and counted and the allocation count
  stays at zero.
- A real VST3 module loads, instantiates, negotiates a stereo bus arrangement, and processes:
  unity by default, exactly 2x with the gain parameter at full, exactly 4x through two chained
  instances, with channel identity preserved end to end.
- A block whose geometry the chain was not built for is passed through bit-for-bit and reported;
  the control thread then rebuilds for it and processing resumes.
- The first chain is built from the geometry the audio thread observed, with no chain having
  existed beforehand -- and `serviceFormatChange` is idempotent when nothing changed.
- A plugin that refuses a channel count fails the rebuild cleanly, leaves nothing published, and
  does not prevent a later rebuild at a format it accepts.
- Republishing five times while the valet thread is processing, then tearing down while it is
  still processing, neither faults nor loses blocks.
- A real third-party plugin -- **ZL Equalizer 2 1.3.1**, JUCE-wrapped, 610 parameters -- loads,
  instantiates and prepares at 48 kHz / 2 ch. That exercises the **split component/controller**
  path end to end: `getControllerClassId`, the second `createInstance`, `ConnectionProxy`, and
  the `MemoryStream` state transfer, none of which our own fixture can reach.
- A plugin carrying a default-active side-chain input negotiates its bus arrangement, and the
  auxiliary bus it keeps reporting as connected is backed with a zeroed buffer rather than null
  pointers. Covered by a named test whose negative control was checked: removing the silence
  backing makes the fixture stamp `-12345.0f` through the output.
- MMCSS "Pro Audio" promotion succeeds on the valet thread (sec. 4.6).
- Source tree is ASCII-only (sec. 6.6), enforced by a tree walk on every `ctest` run.

Proven against the real deployed APO on the development machine:

- The client attaches as a v1 valet and processes real blocks out of `audiodg.exe` at
  48 kHz / 2 ch / 480 frames, with zero timeouts, zero malformed headers and zero reclaims.
- **A real VST3 plugin chain runs in that loop.** `valet_probe --plugin` built a chain for
  48000 Hz x2 ch on first observation and ran 491 of 503 blocks through it, with zero format
  misses and zero dropped parameter edits. The 12 blocks that passed through are the ~120 ms
  before the control thread's first poll tick sees the format.
- **A real third-party plugin runs in that loop.** ZL Equalizer 2 processed 2,991 of 3,003 blocks
  over 30 s with zero timeouts, zero malformed headers, zero format misses and zero dropped
  parameter edits.
- **Zero audio-thread allocations against the real producer**, which the soak test could only
  show against the synthetic king. 1,004 blocks pass-through and 3,003 blocks with ZL Equalizer 2
  both report exactly zero allocations, frees and lock acquisitions. Note what the second figure
  does and does not say: the detector counts everything on the valet thread, so it is evidence
  that *this* plugin is also clean -- another plugin may not be, and that would be its defect
  rather than ours (sec. 7.4.5). Rerun without `--plugin` to separate the two.

**Not** proven -- do not assume any of these:

- Audible processing on real hardware. Everything against the real APO has been at unity gain or
  a flat EQ, on purpose: a real change would alter the machine's actual audio output.
- More than one third-party plugin. ZL Equalizer 2 is proven end to end; it is one JUCE-wrapped
  plugin, and the population it stands for is not obviously well represented by it.
- The split component/controller path in a **test**. It is proven against a real plugin, which is
  a manual step; the automated suite still only sees our single-component fixture. Covering it
  hermetically needs a second plugin fixture.
- Anything at all about *how it sounds*. Every real-APO run has been at unity or with a flat EQ,
  on purpose, and the evidence is block counters rather than audio.
- Plugin state persistence. `IComponent::getState`/`setState` are not called by the engine at
  all; a rebuilt chain starts from defaults, which the format-change test asserts rather than
  works around.
- Multi-bus plugins beyond one main pair plus deactivated extras. Side-chains are now handled
  (see sec. 7 item 20) but nothing *drives* one, and a plugin whose main output is not bus 0 is
  not considered at all.
- Latency compensation. `IAudioProcessor::getLatencySamples` is not read, and protocol v1 has
  nowhere to report it even if it were (sec. 3.7.1 -- the design is zero added latency).
- MIDI/event input. The `IEventList`s are preallocated and always empty.
- Sending automation *into* a plugin. `inputParameterChanges` is preallocated and warmed but
  nothing ever fills it; the tests set values through `IEditController::setParamNormalized`.
- `restartComponent` is queued and then ignored. A plugin that asks for a reconfiguration does
  not get one.
- More than one endpoint at a time, and endpoint switching while attached.
- A real format change (sample rate or channel count) driven by Windows rather than by the
  harness.
- Takeover between two real client processes. The `Stolen` path is covered only by the harness
  forging a foreign valet id.
- A long soak (hours) against the real APO. The longest so far is 30 s.
- ARM64. Declared supported in sec. 6.1, never built. Note that
  `tests/fixtures/aip_test_plugin` hard-codes `x86_64-win` in its bundle layout.

---

## 5. Next actions, in order

1. **`scanner/` -- out-of-process plugin probing (sec. 7.2).** A short-lived executable per scan
   that enumerates `Module::getModulePaths()`, loads each candidate, records class info, and
   reports it back; crash isolation is the whole point, so a plugin that faults must cost one
   scan entry, not the session. `engine::PluginModule` already does the loading half.
2. **`ui/` -- the Qt 6 Widgets shell (sec. 5.1).** The plugin rack, and editor hosting via
   `QWindow::fromWinId()` -> `QWidget::createWindowContainer()`. This is where the deciding
   constraint of the whole stack decision finally gets exercised, so it is worth doing before
   anything else large.

Engine work that the increment above deliberately left out, roughly in order of how much it will
be missed:

- **Plugin state persistence.** `IComponent::getState` on teardown and `setState` on rebuild, so
  a format change does not silently reset every plugin to defaults. This is the most user-visible
  gap.
- **Send automation into a plugin.** `inputParameterChanges` is preallocated but never filled;
  wiring the UI's parameter edits into it is what makes the queue earn its place.
- **Act on `restartComponent`.** At minimum honour `kParamValuesChanged` and
  `kLatencyChanged`; currently every flag is drained and discarded.
- Handle `IAudioProcessor::getLatencySamples` at least by reporting it, even though protocol v1
  cannot compensate for it.

Worth doing at some point, none of it blocking:

- Add `clang-format` to `pixi.toml` with a `format` task. It is listed as project hygiene in
  sec. 6.1 but is not in the toolchain, so the 100-column limit is currently hand-maintained.
- Exercise the probe against a second endpoint and across a Windows-driven format change.
- Decide whether `ValetSupervisor` should expose endpoint hot-swap, which the UI will want.
- sec. 9.6 -- replace the SDK's CMake with an in-house static library. Much less urgent now that
  `cmake/vst3sdk.cmake` gets the build down to five SDK libraries.

---

## 6. Live blockers and open decisions

| # | Item | Owner | Blocks | State |
|---|---|---|---|---|
| 1 | Minimum OS floor (sec. 8.1, leaning Windows 11 only) | project owner | APO rewrite | Open, blocks nothing yet |
| 2 | GFX vs modern registration slots (sec. 8.2, leaning GFX `,2` only) | project owner | APO rewrite | Open, blocks nothing yet |
| 3 | Staged porting plan (sec. 11.4) | project owner | -- | Open |

Nothing is currently blocking client work. The VST3 SDK `URL_HASH` (previously blocker 1) is
pinned in `cmake/vst3sdk.cmake` and recorded in sec. 6.3.2; open item sec. 11.1 is closed.

---

## 7. Implementation decisions not in `design_doc.md`

These were taken while building. They are additive or interpretive, not deviations from the
frozen protocol, but a fresh session would otherwise have to re-derive them.

### 7.1 IPC foundation

1. **`rt/` is a component that sec. 7.1 does not list.** The violation detector needs a real
   translation unit (it replaces global `operator new`/`operator delete`) and the SPSC queue is
   shared by `ipc/` and `engine/`. `aip_rt_hooks` is an **OBJECT** library, not static,
   because replacement operators must land in the final image unconditionally -- a static
   library's members are only pulled in on demand, and the detector would silently do nothing.
   Every executable that wants detection must link it directly.

2. **`BlockStatus::Evicted` is split out from `Stolen`.** Sec. 4.4 step 2 says to detach whenever
   `valetId` stops matching, but `valetId == 0` means the king evicted us on its own timeout with
   *nothing else* claiming the stream. Treating that as `Stolen` made the valet detach
   permanently after a single dropout. `Evicted` re-claims and carries on. No wire change; the
   distinction is in how we read a value the protocol already defines.

3. **The valet waits with a finite 100 ms timeout**, not the reference's `INFINITE`. Sec. 4.4
   explicitly permits any timeout. It is what lets the thread observe a stop request, and what
   bounds how long we can sit unaware of a recoverable eviction. Shutdown latency is one tick.

4. **The supervisor does not re-attach after a takeover** (`SupervisorPolicy::reattachAfterSteal`
   defaults to false). Sec. 4.1 makes displacement intentional; re-attaching would make two
   clients ping-pong an endpoint forever. Set it true only if you actually want to fight.

5. **Tests use `Local\` object names, not `Global\`.** Creating a `Global\` named object needs
   SeCreateGlobalPrivilege, which a non-elevated test runner does not hold. Only the *creating*
   side needs it, so the shipping client is unaffected -- it only ever opens what `audiodg.exe`
   created. The exact sec. 4.2 name construction has its own dedicated test.

6. **`SyntheticKing` reproduces the `smartOpen` bug on purpose** -- the `&&` where `||` was meant
   (sec. 3.7.3). A harness that quietly fixed it would hide the exact behaviour the client has to
   tolerate. There is a test that asserts the stale `sampleRate` is observed.

7. **`RelWithDebInfo` has `/DNDEBUG` stripped** in the root `CMakeLists.txt`, so `assert` is
   actually live in the configuration we develop and test in (sec. 6.4 makes it the only usable
   one).

8. **`ValetSupervisor` has a second constructor taking `ObjectBaseName`**, so the harness can
   drive a synthetic endpoint that has no GUID at all.

### 7.2 Engine

9. **The format comes from the block, never from anywhere else.** Protocol v1 announces the
   endpoint's format nowhere -- there is no negotiation, no capability exchange, only the header
   on each block (sec. 4.5). So `ChainProcessor` records the geometry of *every* block into one
   packed 64-bit atomic, whether or not a chain is running, and `Engine::serviceFormatChange`
   keys off that value. This is why building the first chain and rebuilding after a format change
   are the same code path: on a fresh attach there is no chain to mismatch against, yet the
   format still has to come from somewhere.

10. **A chain that does not match the block is bypassed, not adapted.** Sample rate and channel
    count must match exactly and the frame count must fit; otherwise the block goes back to the
    king untouched and a counter moves. Adapting on the audio thread would mean reallocating,
    which sec. 7.4.1 forbids outright. Audio therefore keeps flowing, unprocessed, across a
    format change -- which is the correct failure mode for something in the system audio path.

11. **The chain ping-pongs between two scratch banks rather than processing in place.** The king
    shares its payload in place, but VST3 makes no promise that a plugin tolerates an aliased
    input and output. Plugin 0 reads the shared mapping and writes bank A, the rest alternate,
    and the final bank is copied back. That is one `memcpy` of a few KB per block, uniform for
    every chain length, and it never hands a plugin the same pointer twice.

12. **Chain retirement uses an epoch counter, not a grace period.** Sec. 7.4.3 step 4 says to
    destroy the replaced chain on the control thread "after a safe grace period"; a counter the
    audio thread raises before loading the chain pointer and lowers on the way out turns that into
    a proof rather than a guess. Both sides are `seq_cst` deliberately -- a release store followed
    by an acquire load is exactly the pattern x86 may reorder. If the audio thread does not leave
    within the grace bound (default 1 s) the old chain is *parked* and freed at teardown: leaking
    it is strictly better than freeing memory the audio thread is reading.

13. **`IComponentHandler` routes by caller, not by contract.** The interface documents every
    method as UI-thread-only and plugins call `performEdit` from their processing thread anyway;
    sec. 7.4.5 makes tolerating that our problem. Calls from the audio thread go onto a
    single-producer ring and are dropped on overflow; calls from anywhere else take a lock and
    append to a vector. They cannot share one queue -- `rt::SpscQueue` has exactly one producer.

14. **`engine::AudioThreadMarker` is separate from `rt::RealtimeGuard` on purpose.** They look
    identical and mean different things: the guard answers "are the sec. 7.4.1 rules in force
    here" and is a debugging instrument compiled out of Release; the marker answers "which thread
    am I on", is load-bearing for the routing in item 13, and is compiled in unconditionally.

15. **Parameter-change queues are *warmed*, not merely sized.** `ParameterChanges::
    setMaxParameters` preallocates the queue objects but each queue's point list is a
    `std::vector` that only stops allocating once it has grown. `PluginInstance::prepare`
    therefore pushes 8 points into each of up to 256 queues and clears them, so the capacity
    survives into the audio thread. Without this the soak test fails the moment a plugin reports
    automation.

16. **`HostProcessData::prepare(component, 0, kSample32)`.** Passing zero buffer samples makes
    the SDK allocate the `AudioBusBuffers` and the channel *pointer* arrays but no sample
    storage, and leaves `channelBufferOwner` false so the pointers can be rewritten per block.
    The samples live in the chain's banks and in the king's mapping.

17. **A plugin that will not take our channel count is refused, not worked around.** Downmixing
    or padding would corrupt the planar payload, which is addressed purely by channel index
    (sec. 4.3). `PluginInstance::prepare` verifies with `getBusArrangement` after
    `setBusArrangements`, because a plugin may return `kResultOk` and then report something else.

18. **`ProcessContext` is synthesised.** There is no transport in a system-wide processor, so the
    context is `kPlaying` with a running sample position, 120 BPM and 4/4 declared valid. Absent
    tempo makes some plugins behave badly; a fabricated stable one makes none of them worse.

19. **The test plugin is ours and is a single component** (`tests/fixtures/aip_test_plugin`).
    Single-component is the configuration in which a plugin can reach `IComponentHandler` from
    its own processing thread, which is the exact behaviour item 13 exists for. It is built by
    hand rather than through `smtg_add_vst3plugin`, because that macro depends on SMTG variables
    set only inside the SDK's own CMake scope -- see the comments in its `CMakeLists.txt`. It
    also carries a deliberately awkward side-chain; see item 20.

20. **`setBusArrangements` describes every bus, not just the main pair.** The first real
    third-party plugin tried, ZL Equalizer 2, refused outright: it carries a default-active
    stereo side-chain, and a host that describes one input bus when the plugin has two is not
    saying "leave the other alone", it is saying something the plugin cannot interpret. Auxiliary
    busses are therefore passed `SpeakerArr::kEmpty`, which is VST3's "not connected" and is
    exactly true -- protocol v1 carries one stream (sec. 4.3). A plugin that refuses `kEmpty`
    falls back to the old main-pair-only call, and `PluginInstance::fullBusNegotiation()` reports
    which happened, because that is a per-plugin quirk the scanner will want.

21. **Every bus we do not drive is backed by a zeroed buffer, not by null.** Accepting `kEmpty`
    does not oblige a plugin to *report* the bus as gone: ZL Equalizer 2 goes on saying its
    side-chain has two channels, and JUCE-wrapped plugins generally do. `HostProcessData` leaves
    those channel pointers null, so a plugin that reads an inactive bus without checking would
    dereference null on the audio thread and take `audiodg.exe` down with it. One `maxFrames`
    buffer of zeros, shared by every channel of every unused bus and written once at `prepare`,
    removes the entire class of crash. A plugin that *writes* to it degrades only itself, which
    is the sec. 7.4.5 bargain.

22. **The fixture reproduces both halves of that on purpose.** It declares a default-active
    stereo side-chain, accepts the host's `kEmpty` and then keeps the bus stereo anyway -- copying
    the real plugin rather than being convenient -- and sums the side-chain into its output, so an
    unchanged output is positive evidence that the host supplied real zeroed memory. The negative
    control was run: removing the silence backing makes the assertion fail with `-12345.0f`
    rather than passing quietly.

---

## 8. Traps already paid for

`design_doc.md` sec. 6.3 lists the build traps found during toolchain verification and SDK
integration. These are the ones found while implementing. Re-discovering any is wasted time.

1. **`vs2022_win-64` breaks the Ninja generator.** It exports
   `CMAKE_GENERATOR_PLATFORM=x64` and `CMAKE_GENERATOR_TOOLSET=v143` for the Visual Studio
   generator it assumes. Ninja Multi-Config rejects a platform specification outright. Blanking
   them is *not* enough -- CMake treats defined-but-empty as specified -- and neither
   `CMakePresets.json`'s `environment` field nor pixi's `env` table can unset a variable. Hence
   the literal `unset ...` prefix on the CMake pixi tasks. Do not remove it.
2. **A failed configure leaves a poisoned `CMakeCache.txt`.** The platform value persists as an
   INTERNAL cache entry, so subsequent attempts fail identically no matter what you fix. Delete
   `build/` when a configure error stops making sense.
3. **Text transport can eat a backslash.** `L"Global\\TOMATL.AUDIO.IPC."` became
   `L"Global\TOMATL.AUDIO.IPC."` during implementation -- an object name that would never have
   matched the APO. Caught only because MSVC warns C4129 on the resulting invalid escape. The
   same thing happens to shell heredocs and editing scripts: a `\n` inside one can arrive as a
   real newline. When writing a string literal containing a backslash, verify the bytes on disk
   afterwards. This is one of the reasons for the ASCII-only rule (sec. 6.6).
4. **A leading dot in a Catch2 tag hides the test.** `[.slow]` kept the sec. 7.4.3 soak test out
   of `catch_discover_tests` entirely -- the single most important test in the suite silently
   never ran. Do not tag tests with a leading dot.
5. **Non-ASCII in a Catch2 test name breaks `ctest`.** The name is registered and then passed
   back as a filter through `argv`; a section sign does not survive, so the test fails with "No
   tests ran" while passing when invoked directly. Now prevented by rule sec. 6.6.
6. **`functiondiscoverykeys_devpkey.h` needs `DEFINE_PROPERTYKEY` already in scope.** The SDK
   header's own `#include <propkeydef.h>` is commented out. It must come *after*
   `<mmdeviceapi.h>`, which pulls the macro in via `<propsys.h>`. Alphabetised includes put it
   first and the build fails inside the SDK header with a wall of C2065/C4430 that says nothing
   about include order. `<initguid.h>` must precede both, in exactly one translation unit.
7. **WRL's `ComPtr` has no `operator*`.** Use `*ptr.Get()`.
8. **Catch2 from conda-forge is a DLL.** Run tests through `pixi run` or the executable dies
   with no output.
9. **The VST3 SDK's `base` target exports `-DRELEASE=1` PUBLICly in `RelWithDebInfo`**, so it
   lands on every one of our translation units that links it. Harmless today -- we define no
   `RELEASE` macro -- but note that it puts the SDK's own `SMTG_ASSERT` into release mode in the
   configuration where we deliberately keep `assert` live (sec. 6.4). Do not introduce a
   `RELEASE` identifier.
10. **A VST3 bundle directory and its DLL want the same path.** `<name>.vst3` is both the
    directory Module::create is handed and the file inside
    `<name>.vst3/Contents/x86_64-win/`. Building the target straight into the inner directory
    (`LIBRARY_OUTPUT_DIRECTORY`) avoids the collision; a POST_BUILD copy does not.
11. **`kQuadro` does not exist** in `Vst::SpeakerArr`. Four channels is `k40Music` (L R Ls Rs) or
    `k40Cine` (L R C Cs).
12. **`pixi run ctest ...` does not build.** Only the `test` task depends on `build`; invoking
    `ctest` directly through pixi runs whatever binary happens to be on disk. A source change you
    are testing may simply not be in it -- which reads as "the test does not catch this" when the
    truth is that the test never saw the change. Use `pixi run test`, and reach for
    `pixi run ctest --preset dev -R ...` only after a build.
13. **A VST3 `moduleinfo.json` is not JSON.** It permits trailing commas, so `json.load` rejects
    the file every real plugin ships. `scanner/` will need the SDK's own parser (or a tolerant
    one) rather than a stock JSON library.
14. **A plugin's declared bus count is not what the host asked for.** After a successful
    `setBusArrangements` that set an auxiliary bus to `kEmpty`, `getBusInfo` may still report it
    with its original channel count. Do not use the reply as confirmation of anything but the
    main busses -- see sec. 7 items 20 and 21.

---

## 9. Keeping this file useful

Update it at the end of any session that changes the answer to "where do we stand".
Specifically:

- Move a milestone in section 2 when it changes state, and say what proved it.
- Add to section 4 when something becomes verified -- and be equally willing to add to the "not
  proven" list. That list is the most valuable part of this file, because it is the part a fresh
  session cannot reconstruct by reading code.
- Rewrite section 5 whenever the next action changes. Keep it to a concrete first step, not a
  goal.
- Add to section 7 whenever a decision is made that the design document does not cover, with the
  reasoning. If a decision turns out to belong in the specification, move it to `design_doc.md`
  and delete it here.
- Add to section 8 whenever something costs more than about fifteen minutes to diagnose.
- Refresh the date at the top.

Keep it ASCII-only (sec. 6.6) -- `pixi run test` checks this file too.
