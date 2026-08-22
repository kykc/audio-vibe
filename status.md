# Project status

**Updated:** 2026-08-22. NeuralAmpModeler restores from a session -- the case where the state is
a *path* rather than a value -- and the negative control settled what the shell can see when that
path stops resolving: nothing at all (item 66). Before that the same day, closing the last boot
loop: a shell that stops existing while it
is attached comes back detached and says why, rather than reattaching into whatever killed it
(sec. 7 item 65). Driven end to end, including the reboot that must *not* be reported as a crash.
Before that the same day, the Session milestone: `config/`, a single YAML file holding the
rack, each plugin's own state, the scan report, the window geometry and the last endpoint --
portable next to the executable, AppData otherwise -- proven by a full round trip through the real
shell, out and back in byte-identical, and by a second start that reused a cached scan instead of
spending two minutes redoing it. Earlier the same day, the Scanner milestone: `scanner/`,
out-of-process plugin probing with a resumable child, proven against fixtures that crash and hang
on purpose and then against all 22 plugins installed on this machine -- three of which would have
killed the shell -- and then wired in behind the shell's plugin picker, which has had a surface
pass, works, and has since been run while the shell was attached without costing a block. Earlier
the same day, a parameter gesture in ZL Equalizer 2's own editor was checked by hand and held,
counter and audio both, which makes this the first day the chain has been *heard*. Before that,
2026-08-21, the UI milestone: the Qt 6 Widgets shell (`ui/`), parameter delivery from a plugin's
editor into its processor, and two real third-party plugin editors hosted side by side while the
chain processes system audio. Before that, the same day: the Engine milestone, the first real
third-party plugin (ZL Equalizer 2, which found and fixed a side-chain bus defect), the sec. 5.1
editor-hosting spike, and the rack/chain split that makes chain mutation non-destructive.

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

**Where we are:** the IPC foundation, the VST3 host, a working GUI and the plugin scanner are all
finished and verified against the real deployed APO -- the scanner against this machine's whole
plugin population instead. A real VST3 plugin chain runs inside the `audiodg.exe` block loop,
driven from a window, with the plugins' own editors on screen, and it all comes back after a
restart. What is missing is that only the fixture plugin's state has been round-tripped -- no
third-party plugin has been through a save and reload yet.

Prove the tree is healthy in one command:

```
pixi run test          # expect: 100% tests passed out of 76, ~7 s, and NOTHING skipped
```

**Read the skip count, not just the pass line.** `pixi run test` (RelWithDebInfo) must skip
nothing. The `release` configuration exists too and skips exactly five -- the ones that need the
sec. 7.4.6 violation detector, which is compiled out of Release by design:

```
pixi run -- cmake --build --preset release && pixi run -- ctest --preset release
```

Both configurations build and both suites pass as of 2026-08-22. See section 8 items 17, 19 and 20
for why that sentence is worth a check rather than an assumption -- twice now, this suite has
reported green while the most important test in it did not run.

Then, on a machine with the old APO installed, prove interop against real hardware:

```
pixi run ui                                 # the shell; pick an endpoint and press Attach
pixi run probe                              # pass-through, console
valet_probe --inspect --plugin <path.vst3>  # load and prepare only; never attaches
valet_probe --plugin <path.vst3> --seconds 30
```

To hand the thing to someone who has none of this installed:

```
pixi run package       # build/package -- a folder that runs on a machine with no pixi,
                       # no Qt and no Visual Studio. Copy it anywhere.
```

The shell also takes a command line, for checking a state without clicking through to it. Note
that the option is `--vst3` and not `--plugin`, because `-plugin` is a reserved Qt option --
see section 8 item 15:

```
aip_ui --vst3 <path.vst3> --editors --attach
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
| **Engine** | **`engine/` -- VST3 host: module loading, the plugin rack, preallocated process data, atomic publication** | **Done for a single linear chain, mutable while running; verified against the real APO** |
| **UI** | **`ui/` -- Qt 6 Widgets shell: endpoint attach, plugin rack, multi-editor hosting** | **Done for a first cut; verified against the real APO with two real plugins** |
| **Scanner** | **`scanner/` -- out-of-process plugin probing: a resumable child, crash and hang isolation, a line-record wire format; wired in behind the shell's plugin picker** | **Done for a first cut; verified against this machine's entire plugin population, and the picker given a surface pass. Scanned once while attached, with no dropouts** |
| **Session** | **`config/` -- one YAML file: the rack, each plugin's own state, the cached scan report, the window, the last endpoint; portable next to the exe, AppData otherwise** | **Done for a first cut; round-tripped through the real shell, and through the suite with a fixture that persists state and refuses a foreign blob. Neither half of a plugin that kills the shell -- while loading, or while processing -- survives into the next start** |
| Installer | `installer/` -- WiX v7 | Not started. `pixi run package` already produces the payload one would carry: a self-contained portable folder |
| APO rewrite | `apo/` -- the rewritten APO | Deferred by design (sec. 7.3, sec. 8) |

"Done for a single linear chain, mutable while running" is deliberate: the engine holds an
ordered rack, publishes a view over it, and lets the rack be inserted into, removed from,
reordered and bypassed while audio flows -- without disturbing the plugins not being touched.
What it does *not* yet do is listed in section 5.

"Done for a first cut" for the UI means: it attaches, it drives the rack, it hosts editors, and it
puts every counter that matters on screen. What it has no control of its own -- the only way to
change a parameter is still the plugin's own editor.
The shell now goes through the scanner to add a plugin: the picker lists scanned classes with
vendor and category, greys out what could not be loaded with the reason in its tooltip, and probes
a browsed bundle in a child before accepting it. It also has a session now: the rack it had, with
every plugin's state, comes back when it starts.

"Done for a first cut" for the scanner means: a scan probes every candidate out of process, a
plugin that faults or hangs costs its own entry and nothing else, and the report carries enough
about each class for a shell to show it. The report is now kept between runs, in the session file,
and each entry is checked against a stamp of the bundle it describes -- total size and newest
write time, read from directory metadata -- so the first Add after a launch probes only what has
been installed or updated since. An unchanged machine costs a directory walk and no child
processes at all.

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
            plugin_chain (a *borrowed* ordered view + ping-pong scratch banks),
            chain_processor (the BlockProcessor: atomic pointer, format check, epoch-based
            retirement), engine (owns the rack; insert/remove/move/bypass, rebuild, publish,
            service). plugin_instance also carries the parameter ring that delivers an editor's
            edits to the processor (item 26)
config/     session.h (the Session struct -- rack entries, cached scan report, window geometry,
            last endpoint -- and capture/apply against an Engine), session_file (the two
            locations, and the YAML reader and writer), base64 (wrapped on the way out,
            whitespace-tolerant on the way back), load_guard (the breadcrumb that lets the next
            start survive a plugin that kills this one while it loads), attach_guard (the mark
            that stops the next start walking back into one that kills it while it processes),
            file_stamp (size and newest write time
            over a
            bundle, from directory metadata, which is what decides whether a cached scan entry is
            still true). No Qt: all of it is testable without a window on screen
ui/         src/ only -- an executable, nothing links it. main (OLE apartment, command line),
            main_window (the shell: endpoint attach, counters, log), session_end_filter (the
            message Windows sends before it ends the session, which is what keeps a reboot from
            being mistaken for a crash), rack_panel (a direct view of
            the engine's rack; no second model), plugin_catalog (the scan report, carried across
            runs in the session file and re-probed per entry only where a bundle's stamp has
            changed, plus the progress dialog that fills it),
            plugin_picker (scanned classes with vendor and category; unusable modules greyed out
            with the reason; Rescan; a browsed bundle probed in a child before it is accepted),
            engine_host (names the GUI thread as *the* control thread; the
            100 ms servicing tick), editor_manager (which editors are open, and the guarantee
            none outlives its plugin), editor_window (one editor, embedded per sec. 5.1),
            plug_frame (IPlugFrame), window_chrome (title-bar icon removal, item 35)
            aip_ui.rc + assets/ -- the application icon, attached to the executable
scanner/    both halves of the out-of-process probe, in one directory because they share a wire
            format. scan_result.h (the SDK-free report the parent deals in), scan_record
            (the line format and its reader), probe_worker (the child half: loads, instantiates,
            prepares, asks for an editor -- the code that is allowed to die), scanner (the parent
            half: spawns aip_scan, feeds it a work list, reads records, times it out, resumes
            past whatever killed it). aip_scan is the executable; aip_scanner is the library,
            and only its parent half is what `ui/` will link
cmake/      vst3sdk.cmake -- the pinned SDK, and every workaround sec. 6.3 calls for.
            yamlcpp.cmake -- yaml-cpp 0.8.0, pinned the same way, static, plus the two policy
            overrides CMake 4 needs to configure it at all (trap 23).
            package.cmake + package_impl.cmake -- the portable folder: a dependency walk for the
            DLLs, an explicit list for the Qt plugins nothing imports, and the redistributable
            CRT. No windeployqt (trap 24)
tests/      76 Catch2 tests. harness/synthetic_king (the sec. 4.7 producer), harness/
            test_processors, harness/wait_for, fixtures/aip_test_plugin (a real VST3 plugin
            built from the SDK), fixtures/aip_hostile_plugin (two that misbehave on purpose --
            one faults inside GetPluginFactory, one never returns from it). conformance,
            config, engine, planar, protocol_layout, realtime_safety, scanner, source_hygiene,
            spsc_queue. The test plugin carries real state -- three parameters behind a magic
            number, and a blob whose magic does not match is refused -- so both halves of a
            session restore are observable rather than inferred
tools/      valet_probe -- console client against the real APO, with --plugin, --inspect and
            --scan (--inspect loads, instantiates and prepares plugins *in the probe* and
            reports bus layout, parameter count, latency and split-vs-single, without attaching
            to anything; --scan does the same work through `scanner/`, one process further out,
            and is the only one of the two safe to point at a plugin you do not trust)
            editor_spike -- the sec. 5.1 de-risking spike: loads a plugin, embeds its editor in
            a Qt window, and can capture the result two ways. The embedding code here is what
            `ui/` should start from
```

Thread split, which is the load-bearing structural decision:

- **Control thread** (`ValetSupervisor`, `engine::Engine`): opens handles, generates the valet id,
  claims the stream, faults in every page, loads modules, activates plugins, builds and publishes
  chains, destroys retired ones, drains plugin callbacks. Allowed to allocate, log and lock. In the
  shell this thread *is the Qt GUI thread* -- see section 7 item 27.
- **Audio thread** (`ValetThread` -> `engine::ChainProcessor`): the sec. 4.4 rendezvous, one
  atomic pointer read, a geometry comparison, and `IAudioProcessor::process` per plugin. Nothing
  else. Real-time safe under sec. 7.4.1.

How a chain gets published, end to end (sec. 7.4.3):

1. `Engine::insertPlugin(index, path)` loads the module (cached by path), instantiates the plugin
   and puts it in the **rack**, which Engine owns. If a format is already known it is prepared
   here, so a plugin that cannot take the current format is rejected without disturbing what is
   already running.
2. The audio thread records the geometry of every block it sees into one packed atomic, whether
   or not a chain is running. That is the *only* place a format comes from -- protocol v1
   announces it nowhere else (sec. 4.5).
3. `Engine::serviceFormatChange` notices a geometry it has not built for, retracts the chain,
   re-prepares every rack entry **in place** for the new geometry, and constructs a
   `PluginChain` -- a borrowed, ordered view over the enabled entries -- with its scratch banks
   page-touched.
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
- **A real plugin editor is hosted in a Qt window (sec. 5.1).** ZL Equalizer 2's editor attaches
  to an HWND we supply, creates its child window, reports 1180 x 752, declares itself resizable,
  and calls back through our `IPlugFrame`. A `PrintWindow` capture shows the whole UI rendered.
  This is the last frozen decision that could have been wrong; it is now measured.
  Both embedding routes work -- `QWindow::fromWinId()` -> `createWindowContainer()` and a
  `QWidget` under `WA_NativeWindow` -- and the first stays the recommendation (sec. 10).
- **Sec. 6.5 is confirmed in both directions.** The same window captured through `QWidget::grab()`
  yields 1 distinct colour and through `PrintWindow` 104. The failure and the remedy are both
  now evidence.
- **Rack mutation while audio is running does not disturb the plugins it is not touching.**
  Adding a second plugin, inserting before an existing one, removing a neighbour, bypassing and
  un-bypassing, and a full re-prepare for a different sample rate all leave every other plugin's
  parameters exactly as they were. Negative control run: making `rebuild` reinstantiate -- the
  old behaviour -- fails the state-survival assertions.
- **Reordering the rack reorders processing.** Checked with a non-commutative pair (a gain and a
  post-gain offset), because two gains would produce identical output either way round and the
  test would prove nothing. Negative control run: making `movePlugin` a no-op fails it.
- **An edit made in a plugin's editor reaches its processor.** The exact call an editor makes --
  `IComponentHandler::performEdit` from a thread that is not the audio thread -- is queued, carried
  across by `serviceParameterEdits`, delivered into `inputParameterChanges` at the top of the next
  block, and changes the output. Asserted with a named test that deliberately never touches the
  fixture's own `setParamNormalized`, because that would move the processor's value directly and
  prove nothing about the host. Negative control run: restoring the old
  everything-goes-to-the-controller routing fails it.
- The same test asserts the edit is **not** echoed back at the controller that made it (item 26).
- Repeated values for one parameter within a block collapse to the last one, so a dragged knob
  costs one queue slot per parameter per block however hard it is dragged.
- **Parameter delivery is allocation-free on the audio thread.** 3,000 blocks with 24 edits pushed
  per block -- more than the drain bound, so the drain hits its limit every block -- report exactly
  zero allocations, frees and locks. This is its own test rather than a rider on the chain soak,
  because delivery put new work *on* the audio thread and the hazard is specific: `addParameterData`
  past the warmed queue count, or `addPoint` past the warmed point count, both allocate inside the
  SDK. Negative control: the same routing revert fails this one too.
- **Both configurations build, and both suites pass.** `RelWithDebInfo` skipping nothing, `Release`
  skipping exactly the five tests that need the violation detector. Release had never been built
  before 2026-08-21 and did not build when first tried (section 8 item 17). Note what this does and
  does not establish: the Release binaries link and pass the suite. Nobody has run the Release
  shell against the real APO, and its allocation behaviour is *unobservable* by construction --
  `AIP_RT_CHECKS` off means the global `operator new`/`delete` replacements are gone, so the
  counters that make the sec. 7.4.3 criterion checkable exist only in RelWithDebInfo.
- MMCSS "Pro Audio" promotion succeeds on the valet thread (sec. 4.6).
- **A plugin that faults costs one entry, not the scan.** `aip_crash_plugin` dereferences null
  inside `GetPluginFactory` -- in the loader's own call, before any of our code is on the stack.
  The scan records it as `Crashed`, starts a second child, and probes the plugin behind it
  properly. Negative control checked directly rather than by inference: run `aip_scan` on that
  bundle by hand and the process dies with an access violation after emitting exactly its `begin`
  record, which is the wreckage the parent is written to read.
- **A plugin that hangs costs one entry either.** `aip_hang_plugin` never returns from
  `GetPluginFactory`; with the deadline turned down to 1.5 s the child is terminated, the entry is
  recorded as `TimedOut`, and the scan continues in a fresh child. Same negative control: by hand,
  the process sits there until something kills it.
- Every requested path gets an entry whatever happens -- including when the child executable
  cannot be started at all -- and progress is reported as entries land rather than at the end.
- A record field survives every byte a plugin might put in a name: backslashes, newlines, a NUL,
  and bytes outside ASCII all round-trip, and the escaped form stays inside 0x20-0x7E.
- A truncated record stream names the module that was in flight, which is the mechanism the
  crash attribution rests on.
- **A plugin comes back holding what it held.** Two instances of the fixture, given different
  non-default parameters, captured, written to YAML, read back, and rebuilt in a *fresh* Engine:
  each returns with its own value and the one parameter nobody touched is still at its default.
  The fixture implements real `getState`/`setState` for this, so the assertion is about the
  plugin's state and not about our own bookkeeping. Negative control run: making
  `PluginInstance::loadState` a no-op fails it.
- A plugin that **refuses** a state blob is still loaded, reported, and left at its defaults --
  the case a user meets after updating a plugin whose format changed. The fixture checks a magic
  number to make that reachable on demand. Negative control: the same no-op fails this one too,
  from the other side.
- A session naming a plugin that is no longer installed loses that entry and *only* that entry:
  the ones after it are still built, and built at the right positions rather than around a hole.
- Base64 round-trips every length from 0 to 256 bytes over every byte value -- a NUL, a byte that
  is not valid UTF-8 and a newline included -- survives being wrapped and unwrapped, and rejects
  a truncated quantum, a foreign character, and padding in the middle rather than inventing
  bytes for any of them.
- The cached scan report survives the file: status, per-class detail and the stamp, for a module
  that loaded and for one that hung. Keeping the *failures* matters as much as keeping the
  successes -- re-probing a plugin that hangs costs the full 60-second deadline every time.
- The breadcrumb names what was being loaded, is gone once the load returns, is gone when the
  guard is destroyed with a mark outstanding -- a clean shutdown mid-load is not a crash -- and is
  *consumed* when it is read, so an entry it blocked can be tried again by clearing one flag.
- The attach mark is written when the shell attaches, is gone when it detaches and gone when the
  guard is destroyed, and is *consumed* when it is read. On the policy it feeds: an automatic
  reattach is refused for a previous run that vanished while attached and for an endpoint that has
  gone, allowed for an ordinary one, and a session that was closed detached produces no decision
  to explain at all.
- What killed the last start is blocked and nothing else is; a module the scan report calls
  crashed or timed out is blocked without a breadcrumb at all; a blocked entry is skipped,
  reported, and still written back to the file with its reason.
- A cached entry with no stamp is dropped on read rather than trusted, and an invalid stamp never
  compares equal even to itself. That is the one property a cache must have here: an entry that
  cannot be checked against the file system has to be re-probed, not believed.
- A bundle stamps identically twice and differently after one byte is appended to one file inside
  it -- checked by copying the fixture bundle and editing the copy.
- A session survives being written and read back: rack order, bypass flags, both state blobs,
  window geometry and endpoint id. A file from a future format version is refused with the
  version in the message rather than half-read; an *empty* file is not an error, because an empty
  file next to the executable is the documented way to ask for portable mode.
- Source tree is ASCII-only (sec. 6.6), enforced by a tree walk on every `ctest` run -- `ui/`,
  `scanner/` and `config/` included.

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
- **The shell does all of it, from a window.** `aip_ui` enumerated 8 render endpoints, attached to
  the default one, built a chain for 48000 Hz x2 ch on first observation, and ran ~94 blocks/s
  through a two-plugin rack (ZL Equalizer 2 -> NeuralAmpModeler) with zero timeouts, zero malformed
  headers, zero format misses, zero dropped edits, zero stranded plugins and zero audio-thread
  allocations, frees or locks. Both plugins' editors were open the whole time.
- **Two real plugin editors are hosted at once, and both fill their windows.** ZL Equalizer 2 at
  1180 x 752 and NeuralAmpModeler at 600 x 400, each with one child HWND under the handle we gave
  it, both reporting `IPlugViewContentScaleSupport`. ZL Equalizer 2's analyser was drawing the live
  spectrum of the machine's own audio, which is the first evidence in this project that a plugin is
  *receiving* the system stream rather than merely being called with it.
- **A graceful shutdown with editors open while attached exits 0.** WM_CLOSE to the shell with two
  editors on screen and the valet thread processing: no fault, no hang. This is the ordering
  `~MainWindow` exists for -- editors released before the engine that owns the plugins behind them.
- **Zero audio-thread allocations against the real producer**, which the soak test could only
  show against the synthetic king. 1,004 blocks pass-through and 3,003 blocks with ZL Equalizer 2
  both report exactly zero allocations, frees and lock acquisitions. Note what the second figure
  does and does not say: the detector counts everything on the valet thread, so it is evidence
  that *this* plugin is also clean -- another plugin may not be, and that would be its defect
  rather than ours (sec. 7.4.5). Rerun without `--plugin` to separate the two.
- **A parameter gesture in a real plugin's own editor, and the first audible processing.** Checked
  by hand on 2026-08-22: with the shell attached, a control dragged in ZL Equalizer 2's editor made
  `parameters delivered` climb in the Counters group, and the machine's own audio changed as
  expected. Two things at once. The counter closes the last untested link in the `performEdit` path
  -- the one place where a real editor's calling thread could have been classified as the audio
  thread and routed the wrong way (sec. 7 item 26). The audio is the first evidence in this project
  that the chain does something *heard*, rather than something counted; every real-APO run before
  this was deliberately at unity or flat. Note the standard of evidence: a report from the project
  owner at first glance, not a measurement and not a test. The automated suite still exercises the
  edit path against the fixture only, and nothing has compared output against a reference.
- **The scanner survives this machine's real plugin population, which the fixtures could only
  simulate.** `valet_probe --scan` over all 22 installed bundles on 2026-08-22: 17 usable, 2
  unloadable (`LoadLibraryW` denied), **1 crashed and 2 hung**, in 4 child processes -- one for the
  scan plus exactly one more per abnormal exit, which is the cost model the design predicts. Read
  that figure the right way round: three of this machine's own plugins would have taken the shell
  down or wedged it if `plugin_picker` had loaded them in-process, and one of them is a plugin the
  project owner works on. The scanner is not insurance against a hypothetical hostile plugin; the
  population it has to survive is already installed here.
- **The picker drives the scanner from the shell.** A surface pass by the project owner on
  2026-08-22: Add opens the picker, a scan runs behind its progress dialog, the list comes out of
  the report, and a plugin chosen from it is added. Reported as working as expected. Note the
  standard of evidence -- a quick look, not a systematic pass, and the awkward cases below were not
  reached.
- **A scan while attached does not interrupt the audio.** Checked by hand by the project owner on
  2026-08-22: with the shell attached and a chain running, a full scan of this machine's plugin
  population -- around two minutes, most of it the two 60 s timeouts -- produced no audible
  artifacts and left `timeouts` at zero. That is the evidence item 44 was missing. The progress
  dialog's hand-pumped event loop does keep `EngineHost`'s servicing tick firing, and starting a
  succession of child processes while loading every plugin on the machine does not perturb the
  valet thread. Note the standard of evidence, and note what it does not cover: no format change
  was driven during the scan, so the tick is shown to keep *running*, not shown to still act on a
  rebuild.
- **The shell saves a session and starts from it.** Checked end to end on 2026-08-22, driving
  the real `aip_ui`: started with `--vst3` and a fresh `--config`, closed with a WM_CLOSE, started
  again with *only* `--config`, and closed again. The second file is byte-identical to the first
  -- same rack, same class id, same state blob, same window geometry, same endpoint -- which it
  could only be if the whole session had been read back into a real Engine and captured out of it
  again. Note what this does *not* cover: the fixture plugin was the only one in the rack.
- **A real third-party plugin's state survives a restart.** Checked by the project owner on
  2026-08-22 with ZL Equalizer 2: set up, closed, reopened, state intact. The file says what that
  cost -- around 37 kB of base64 holding the plugin's own serialization, which is JUCE's
  `ParaState` XML of all 610 parameters followed by its `JUCEPrivateData` block. That is a real
  plugin's own format, versioned and opaque to us, so this is the first evidence that what
  round-trips is the *plugin's* state and not our bookkeeping about it.
- **The split component/controller path ran, and the plugin declined.** ZL Equalizer 2 is a split
  plugin, and there is no `controllerState` in the file: its `IEditController::getState` gave
  nothing back, which is normal for a JUCE wrapper -- JUCE keeps everything, editor size included,
  in the component blob. So the decline is handled and costs nothing. What has still never
  happened is a *non-empty* controller blob making the round trip; see below.
- **The portable folder runs with none of the build environment present.** `pixi run package`
  on 2026-08-22 produced 30 DLLs plus the Qt plugins, 71 MB; copied out of the build tree to a
  temporary directory and run from a shell with *no* pixi environment on `PATH`, `aip_ui.exe`
  came up, enumerated the endpoint and rewrote its own `aip_config.yaml` in place -- which is both
  halves at once, since a session file next to the executable is what portable mode means.
  `aip_scan.exe` runs from the same folder too, probing the fixture and printing its wire format,
  so the scanner has its child where it looks for it (item 42).
- **A plugin that kills the shell costs one start, not the application.** Driven end to end on
  2026-08-22 with `aip_crash_plugin`, the fixture that faults inside `GetPluginFactory` -- in the
  loader's own call, before any of our code is on the stack. A session naming it takes the shell
  down (exit 139) and leaves the breadcrumb holding its path; the next start reads it, skips that
  entry, comes up, and writes `blocked: true` with the reason into the file; the start after that
  comes up too. Clearing `blocked` by hand really does try it again -- it crashes again, which is
  the negative control that proves the entry is retried rather than permanently dropped -- and the
  start after *that* re-blocks it. The whole cycle, both directions.
- **The scan report prevents the first crash as well.** Same fixture, a session with no breadcrumb
  but a catalog entry marking the module `crashed`: the shell comes up on the *first* start with
  the entry blocked and `the plugin scan reports it as crashed` written into the file. That is the
  scanner's value being spent where it was always meant to be -- a child process died finding this
  out, once, and nothing here has to die again to act on it.
- **The shell reattaches to what it was attached to.** Checked end to end on 2026-08-22 against
  the real APO: started with `--attach`, closed, and the file recorded `attached: true`; started
  again with *no* flags and closed, and the file still said `attached: true` -- which it can only
  do if the shell attached on its own, because the flag is written from what the link is actually
  doing at closing time rather than from what the file said on the way in. Negative control run:
  editing the file to `attached: false` and starting again leaves it false.
- **A shell that vanished while attached does not take the machine's audio again by itself.**
  Driven end to end on 2026-08-22 against the real APO, on an endpoint nothing was playing on.
  Attach, and the mark appears next to the session file naming the endpoint. `TerminateProcess`,
  which leaves exactly what a fault on the audio thread leaves -- no destructors, no `closeEvent`,
  the mark still on disk. The next start comes up **detached**, consumes the mark, says why in the
  log and in a dialog, and the start after *that* reattaches on its own -- the negative control,
  and what makes this one refusal rather than a shell that has forgotten how to attach.
- **A reboot is not reported as a crash.** The same run, driven with the messages Windows itself
  sends. `WM_QUERYENDSESSION` delivered to the shell's window clears the mark while the process is
  still alive and still attached, so being killed after it -- which is what a shutdown does to an
  application that answers too slowly -- leaves nothing behind, and the next start reattaches
  without a word. `WM_ENDSESSION` with `wParam` false puts the mark back, because a shutdown that
  was called off leaves a shell that is still attached and still worth protecting. This is the
  false positive the project owner named before the code was written (item 65).
- **A plugin whose state is a *path* comes back working -- and its dependency going missing is
  invisible to us.** NeuralAmpModeler, checked on 2026-08-22. Its state is 219 bytes: an id, a
  version, the model's absolute path as a length-prefixed string, an empty IR path, and ten
  doubles. The project owner confirmed by hand that picking a model, closing and reopening brings
  the same model back, and the same restore was then driven from a session file with the editor
  open to watch it happen.

  The negative control is the half worth having, and it was run without touching a real model
  file: the path *inside the blob* was rewritten to a directory that does not exist, same length,
  so nothing else in the state moved. The plugin restores, the parameters restore, the shell says
  `1 of 1 plugin(s) restored` and reports no problem at all. The only thing anywhere that says
  otherwise is inside the plugin's own editor, which relabels its model slot `(FAILED) <name>`.

  And the state it hands back on the way out is **byte-identical to the one that failed**. That
  retires the obvious host-side diagnostic before it was written: asking for the state again after
  `setState` and comparing detects nothing here, because a plugin that could not find its model
  still faithfully remembers where it looked. See item 66.
- **The scan report is reused instead of redone.** Measured on 2026-08-22 with the real
  `aip_ui --scan` against this machine's plugin population: a cold start with no session file
  probed all of them and wrote 21 cached entries; a second start from that file produced the same
  21 entries with the shell alive for twelve seconds, which a full scan cannot do -- it needs
  around 124 s here, most of it the two plugins that hang. Confirmed again by the project owner
  the same day, from the real AppData session rather than a throwaway one: 21 catalog entries came
  back, statuses and all -- including the module that crashes, carrying its own
  `exited (code 0xC0000005) while probing this plugin`, which is exactly the entry that is most
  expensive to rediscover. The twenty-second entry is a *dangling
  symlink* in the VST3 folder pointing at a build output that no longer exists; it cannot be
  stamped, so it is re-probed at every start and reported unusable, which is both correct and
  cheap -- the load fails immediately.
- The crashed plugin exited with code 3 -- a CRT `abort`, not a fault -- and it cost milliseconds.
  That is `suppressCrashDialogs` doing its job: without `_set_abort_behavior`, `abort` puts up a
  modal box and the crash becomes a 60-second timeout instead.

**Not** proven -- do not assume any of these:

- **Audio quality.** A single EQ move sounded right at first glance (see above), which establishes
  that the chain is audible and not obviously broken. It says nothing about correctness: no null
  test against a reference, no check for clicks at a rebuild or a bypass toggle, no sustained
  listening. The project owner has this queued as a separate extensive pass.
- A parameter gesture on a plugin *other than* ZL Equalizer 2. One real editor is now checked (see
  above); NeuralAmpModeler and anything iPlug2-wrapped is not.
- **That the two plugins which timed out would ever have finished.** Virtuoso and
  `fx_multizone_gpua_cu_wrapped` made no progress for 60 s and were terminated; nobody has waited
  longer to find out whether they are slow or stuck. The 60 s default is a judgement, not a
  measurement, and it is the one number in `scanner/` most likely to be wrong -- too low and a
  working plugin is reported broken, too high and a scan of this machine takes minutes. It already
  does: that scan took 124 s, of which 120 s was those two waits.
- **The picker under anything but a quick pass.** A surface test on 2026-08-22 found it working
  (see above), which retires the question of whether the Qt layer is wired up at all. It does not
  retire the awkward cases, none of which a quick pass would reach: Cancel part way through a scan,
  a scan started while a previous one is somehow still running, a module exposing more than one
  class, and the greyed-out entries -- which need a machine whose broken plugins are the ones being
  looked at rather than scrolled past.
- **That a format change is acted on during a scan.** A scan while attached is now known not to
  cost audio (see above), which was the half that could have caused a dropout. The other half is
  untried: nothing has driven a format change while a scan was running, so item 44's claim that the
  control plane keeps *working* -- as opposed to merely not blocking the audio -- remains reasoning.
  It needs a rate change forced from Windows mid-scan.
- That a plugin cannot corrupt the record stream. Records travel on a private pipe and the child's
  stdout and stderr go to the null device, which is the defence; it has not been tested against a
  plugin that actually prints.
- That asking for an editor view is safe in a process with no message loop. Every plugin scanned
  here tolerated `createView` followed by a release, but the child has no UI thread and a plugin
  entitled to expect one has not been met yet. `--no-editor` exists for when it is.
- More than two third-party plugins *hosted*. The scanner has now instantiated and prepared
  seventeen of this machine's plugins, which is real coverage of the loading path -- but hosting is
  a different matter, and ZL Equalizer 2 and NeuralAmpModeler are still the only two carried
  through a chain and an editor. They are one JUCE-wrapped and one iPlug2-wrapped plugin, and the
  population they stand for is not obviously well represented by two.
- Any rack mutation *from the shell* while attached. Insert, remove, move and bypass are proven by
  test at the engine level and the buttons are wired to exactly those calls, but no one has clicked
  them with audio flowing. Removing a plugin whose editor is open is the interesting one, because it
  is the one case where the UI carries an ordering obligation the engine cannot enforce (item 28).
- A plugin that has no editor, or whose editor refuses our HWND. Both are handled and reported
  through the log rather than assumed away, and neither has been seen.
- A plugin that does not implement `IPlugViewContentScaleSupport`. Both plugins tried do, so the
  unit conversion written for the other case is from the specification and has never run.
- Endpoint switching from the shell. `Refresh` and the combo box are disabled while attached, which
  makes the question "detach, then attach elsewhere" -- untried, and no reason to think it fails.
- Two shells at once, which is the real `Stolen` test (see below).
- Anything about how the shell behaves on a high-DPI monitor other than this machine's, or across a
  monitor change while an editor is open. `setContentScaleFactor` is sent once, at embed time, and
  nothing listens for a screen change.
- The split component/controller path in a **test**. It is proven against a real plugin, which is
  a manual step; the automated suite still only sees our single-component fixture. Covering it
  hermetically needs a second plugin fixture.
- **That a plugin which lost its external dependency still passes audio, and what it sounds
  like.** The restore is now checked in both directions (see above), but both runs were detached:
  nothing was attached while a model was missing, so what a failed model does to the chain is
  inference -- NAM prepares and processes, and the expectation is a signal that passes through
  unmodelled. Nobody has heard it.
- **A non-empty controller blob.** The split path runs and a decline is handled (see above), but
  no plugin met so far has actually returned controller state, so the restore half of it --
  `IEditController::setState` with real bytes -- has never executed. Every plugin in the automated
  suite is single-component, where the controller blob is skipped by design (item 48), so the
  suite cannot cover this either.
- **Reattaching to an endpoint whose device has changed under it.** The reattach is suppressed
  when the endpoint id is gone from the enumeration, which is the unplugged case. What has not
  been tried is the same id coming back at a different format, or a device that enumerates but
  will not produce blocks. Neither has a reason to fail; neither has been seen.
- **A real plugin faulting inside `process`.** What the fault would cost is now bounded -- the
  next start comes up detached instead of walking back into it (item 65) -- and that was driven
  with `TerminateProcess`, which leaves behind exactly what a fault leaves. What has not happened
  is the trigger: no plugin on this machine has ever faulted while processing, so the response is
  proven and the event it responds to is still hypothetical. Note also what is *not* claimed: the
  crash still happens, and still takes that run's audio with it. Only the loop is closed.
- **That a real Windows shutdown clears the attach mark.** `WM_QUERYENDSESSION` was delivered by
  hand with `SendMessage`, and the shell has no way to tell that from the real one -- but a real
  restart with the shell left open has not been sat through, and neither has a power cut, which
  is the one end that genuinely does report a crash that did not happen.
- **That the portable folder runs on another machine.** It runs here without the build
  environment, which is the strongest thing that can be checked *here* -- but this machine has
  Visual Studio, the Windows SDK and a Qt installation somewhere on it, and none of that can be
  fully hidden from a process by removing a directory from `PATH`. The claim the folder is for is
  "a machine that has none of this", and that machine has not been tried. The likeliest thing to
  be missing is a CRT the redist copy does not cover.
- That the breadcrumb survives losing power. It is flushed to the operating system, which outlives
  a process that faults or is killed; it is not `fsync`, and it is not meant to be. The failure
  being defended against is a plugin, not a power cut.
- **That the stamp catches every way a plugin can change.** Size plus newest write time is a
  heuristic. It cannot miss an installer or an updater, because both write files; what it would
  miss is an edit that preserves both, which nothing that installs software does. Untested against
  a real plugin update -- only against a byte appended to a copy of the fixture.
- That the catalog behaves when a plugin is installed *while the shell is open*. The check happens
  once per run, on the first Add; Rescan is the answer and always was, but nobody has confirmed
  that the cached path does not make Rescan look like it did nothing.
- That a session is saved when the shell does *not* close cleanly. The file is written from
  `closeEvent` and nowhere else, so a crash or a kill loses whatever changed since the last
  clean exit.
- Multi-bus plugins beyond one main pair plus deactivated extras. Side-chains are now handled
  (see sec. 7 item 20) but nothing *drives* one, and a plugin whose main output is not bus 0 is
  not considered at all.
- Latency compensation. `IAudioProcessor::getLatencySamples` is not read, and protocol v1 has
  nowhere to report it even if it were (sec. 3.7.1 -- the design is zero added latency).
- MIDI/event input. The `IEventList`s are preallocated and always empty.
- *Host-originated* automation. `inputParameterChanges` is now filled -- by the plugin's own editor,
  through `performEdit` -- but nothing in this project writes a parameter of its own accord. There
  is no host automation lane, no preset recall, no generic parameter list.
- `beginEdit`/`endEdit` are drained and discarded. They bracket a gesture, which only matters to a
  host that keeps its own automation state.
- `restartComponent` is queued and then ignored. A plugin that asks for a reconfiguration does
  not get one.
- More than one endpoint at a time, and endpoint switching while attached.
- A real format change (sample rate or channel count) driven by Windows rather than by the
  harness.
- Takeover between two real client processes. The `Stolen` path is covered only by the harness
  forging a foreign valet id.
- A long soak (hours) against the real APO. The longest so far is a few minutes, from the shell.
  The shell is the natural place to run one now, because it shows the allocation counters live.
- ARM64. Declared supported in sec. 6.1, never built. Note that
  `tests/fixtures/aip_test_plugin` hard-codes `x86_64-win` in its bundle layout.

---

## 4a. How to assess stability, and why in RelWithDebInfo

`pixi run ui`, left attached with plugins loaded, is the right primary instrument -- and
RelWithDebInfo is the right configuration to do it in. That is not a compromise forced by
sec. 6.4; it is the better choice on the merits:

- `assert` is live (the root `CMakeLists.txt` strips `/DNDEBUG` from RelWithDebInfo), so a broken
  invariant aborts loudly instead of continuing quietly into something worse.
- `AIP_RT_CHECKS` is on, so the global `operator new`/`delete` replacements are in the image and
  the audio-thread allocation, free and lock counters are live -- and on screen in the Counters
  group. In Release those numbers do not exist. **A stability run in Release can tell you it did
  not crash; only RelWithDebInfo can tell you it stayed real-time safe.**
- Full PDBs, so when a plugin faults there is a usable stack rather than a hex address. The plugins
  are third-party and outside our control (sec. 7.4.5); being able to tell "their frame" from
  "our frame" is the whole difference between a bug report and a shrug.
- `/O2` is on, so it is not a slow build pretending to be a fast one. The one thing it is *not* is
  the shipping configuration -- see the caveat below.

What the shell shows continuously, and what each number means when it moves:

| Reading | Healthy | If it moves |
|---|---|---|
| `audio thread: allocations / frees / locks` | exactly 0, forever | a sec. 7.4.1 violation; the run is a failure however smooth it sounded |
| `timeouts` | 0 | the valet missed the king's 1000 ms window -- a system-wide dropout (sec. 3.7.1) |
| `malformed` | 0 | a header we rejected; benign for us, but says something changed upstream |
| `format misses` | 0 in steady state | a geometry the chain was not built for; audio passed through unprocessed |
| `passed through` | small and constant | grows only across a rebuild; growing steadily means no chain is running |
| `plugin edits dropped` | 0 | the control thread is not draining often enough |
| `stranded plugins` | 0 | the audio thread failed to release a chain within the grace bound |
| `reclaims` | 0, or rare | the king evicted us and we re-claimed; recoverable, but note it |

What `pixi run ui` does **not** cover, and so must not be mistaken for a clean bill of health:

- **Resident set.** The sec. 7.4.3 criterion is zero allocations *and* flat RSS. The shell shows
  the first and not the second, so a long soak still needs an external watch on the process's
  working set. Making the shell show it is the obvious small improvement and is not done.
- **Anything but steady state on one endpoint at one format.** Everything in the "not proven" list
  above stays unproven no matter how long the window is left open: no format change, no endpoint
  switch, no takeover, no rack mutation while attached.
- **Release behaviour.** Nothing about a RelWithDebInfo run transfers to Release automatically:
  removing the allocation hooks changes allocator behaviour and timing, and `assert` going away
  changes what happens after a violated invariant rather than preventing it. Build and run Release
  before believing anything about it -- and remember that the instrument that would have caught a
  real-time defect is exactly what is missing there.
- **An ungraceful exit is not a neutral event.** Killing the process while attached leaves the king
  waiting out its full 1000 ms, which is an audible system-wide dropout before it recovers. Close
  the window, or press Detach; reach for the task manager only when testing that path deliberately.

---

## 5. Next actions, in order

The de-risking steps that used to head this list -- a real third-party plugin, editor hosting, and
a UI to drive them -- are all done, and all held. What remains is ordinary construction.

The manual checks that used to head this list are done as well, both on 2026-08-22. A knob moved
in ZL Equalizer 2's own editor reached the processor and was heard, and a scan run while the shell
was attached cost no audible artifacts and no timeouts -- the last outstanding check that could
have bitten audibly. Both are in section 4 now. So is the session file, built the same day
and checked twice by hand: ZL Equalizer 2's own state survives a restart, and the cached scan
report comes back instead of costing two minutes. The shell is usable daily as of now.

A session that crashes on load no longer costs the application -- section 7 item 56 -- and as of
2026-08-22 neither does one that crashes while *processing*: the shell writes a mark while it is
attached, and a start that finds one left behind comes up detached and says so, in a dialog rather
than only in the log (item 65). The project owner chose that over timing the attach, and the
deciding argument for how it is built was the false positive -- an application that announces a
crash after an ordinary reboot is one nobody believes a second time.

Restoring NeuralAmpModeler was the last item here and is done (section 4, and item 66): a state
that is a *path* comes back working, and a path that no longer resolves is invisible to the host
by construction. Nothing follows from it that is worth building, which is why the list below is
now ordinary component work with nothing numbered above it.

Engine work still outstanding, roughly in order of how much it will be missed:

1. **Act on `restartComponent`.** At minimum honour `kParamValuesChanged` and
   `kLatencyChanged`; currently every flag is drained and discarded.
2. Handle `IAudioProcessor::getLatencySamples` at least by reporting it, even though protocol v1
   cannot compensate for it.

Scanner work still outstanding:

- **Find out what the two timed-out plugins are actually doing**, before tuning the 60 s deadline
  around a guess. Attach a debugger to the child, or run `aip_scan` on one of them by hand and see
  where it sits. The answer decides whether the default is too low or whether those plugins are
  simply unscannable, and it is the difference between a 4-second scan and a 124-second one here.
- Report the cache in the picker. It says "21 plugin(s), 4 unusable" and not how many of those
  came from the cache or when they were probed, so a user who suspects the shell is wrong about a
  plugin has nothing to look at before pressing Rescan.
- Report which classes a module exposes when there is more than one. The report already carries
  them; nothing consumes the plural case, and no plugin here has exercised it.
- Consider scanning in parallel. One child at a time is the simple, correct shape and 17 sound
  plugins cost about 4 seconds -- but a machine with a hundred, or one where several hang, would
  benefit. Not urgent, and it makes the crash accounting materially harder.

UI work still outstanding, none of it blocking:

- **Resident set on screen, next to the allocation counters.** It is the other half of the
  sec. 7.4.3 acceptance criterion and the shell currently shows only one half, which makes a long
  soak weaker evidence than it needs to be. `GetProcessMemoryInfo` and `psapi`, which `tests/`
  already links. Cheap, and it is the difference between "no allocations on the audio thread" and
  "nothing is leaking anywhere".
- Uptime and peak-since-reset alongside the counters, so a soak run documents itself.
- A rack that shows more than a line of text per plugin: vendor, category, and the parameter count
  are all already available from `PluginModule`/`PluginInstance`.
- Drag-and-drop reordering, and dropping a `.vst3` onto the window to add it.
- Endpoint hot-swap while attached, which needs `ValetSupervisor` to grow it (see below).
- The sec. 5.2 "EQ/plot widgets" the design document mentions for `ui/`. Nothing needs them yet;
  they belong with whatever the project's own processing turns out to be, not with plugin hosting.

Worth doing at some point, none of it blocking:

- **Save the session from `WM_QUERYENDSESSION` too, not only from `closeEvent`.** The shell now
  hears that message (item 65), and a restart with it left open still loses whatever changed since
  the last clean exit -- which is section 4's last "not proven" line about saving. What needs
  thinking about first is the budget: the message has a few seconds before Windows stops waiting,
  and `capture` asks every plugin in the rack for its state.
- Decide whether `aip_ui` should set its own working directory. Plugins write files into it
  (trap 22 -- and the culprit is now known: NeuralAmpModeler, on instantiation), and today that is
  wherever the shell was launched -- the repository root, under `pixi run ui`, where it turns the
  ASCII hygiene test red. The scanner child was fixed this way (item 45); the shell was not, because
  `--vst3` accepts a relative path and moving the current directory would quietly change what one
  resolves to. Doing it after argument parsing is the obvious answer if it is worth doing.
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

11a. **Bus arrangement is negotiated in three tiers, and a plugin wider than the stream is padded
    rather than refused.** In order, each tried only when the one before it fails:

    1. the VST3 arrangement the endpoint's `dwChannelMask` names
    2. `speakerArrangementFor(channelCount)` -- right cardinality, guessed speaker roles
    3. the plugin's own fixed arrangement, if it is at least as wide as the stream, with the
       surplus channels fed silence and their outputs discarded

    Tier 3 is what makes a plugin built around one fixed wide bus loadable at all; Voxengo's are
    the usual specimens, and before this they were refused outright. It goes one way only -- a
    plugin *narrower* than the stream would have to drop channels, so that stays a refusal.

    Two things about tier 3 are easy to get wrong and are covered by tests that fail without
    them. `PluginInstance::process` must write `inputChannelCount()` pointers, not
    `format().channelCount`: `HostProcessData` sizes those arrays from `getBusInfo` *after* the
    negotiation, so a short loop leaves null pointers in the tail for the plugin to dereference.
    And the padding must be re-zeroed before *each* plugin, not filled once, because every plugin
    writes its own full output width into the destination bank -- so what was silence holds the
    previous plugin's output by the time the next one reads it.

    The banks are therefore sized to the widest bus any plugin in the chain settled on, computed
    at construction, and `kMaxChannels` rose to 32 to bound what a plugin can demand. The
    remaining hazard is a plugin whose detector links across the bus: it sees the padding as
    silence and acts more gently than it should. `PluginInstance::padded()` and the scanner's
    `class.padded` flag exist so that is discoverable rather than mysterious.

11b. **The endpoint's speaker layout is read from the device, not carried on the wire.** Protocol
    v1 has no channel-order field and sec. 4 is frozen, so tier 1 above would look like it needs
    a v2. It does not: the object names are derived from `PKEY_AudioEndpoint_GUID` (sec. 4.2), so
    a valet that can attach at all already knows which endpoint to ask, and
    `PKEY_AudioEngine_DeviceFormat` on that device's property store carries the
    `WAVEFORMATEXTENSIBLE`. `ipc::RenderEndpoint::channelMask` is that value; `Engine::
    setChannelMask` holds it across format changes.

    The mask is a *hint*, guarded so it can never be wrong rather than merely inaccurate: it is
    discarded unless its population count equals the channel count in the block header, which is
    read every block and is the authority. A stale or absent mask costs the tier-1 attempt and
    nothing else.

    The mapping itself is the identity on the bits -- VST3 numbered its first eighteen speakers
    in the `SPEAKER_*` order -- with two corrections: masks with bits above 17 are discarded, and
    one channel returns `kMono`, because Windows spells mono `SPEAKER_FRONT_CENTER` and that bit
    is VST3's *centre* channel of a surround layout. This also fixed a standing wrongness: a
    Windows 7.1 endpoint reports `KSAUDIO_SPEAKER_7POINT1_SURROUND` (side speakers, VST3
    `k71Music`), and the count-based guess reaches for `k71Cine`, which puts the extra pair at
    front-of-centre instead.

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

23. **The rack owns the plugins; a chain only borrows them.** `Engine` holds an ordered rack of
    `PluginInstance`s that outlives any number of published chains, and `PluginChain` is a view of
    raw pointers over the enabled subset. The first version had the chain own its plugins, which
    meant every mutation went through `rebuild` and `rebuild` reconstructed everything -- so
    adding a second plugin silently reset the first one to its defaults, and so did a sample-rate
    change. That is indistinguishable from a UI bug and would have made `ui/` unbuildable on top.

    The fix is ownership, not serialization. A format change now re-prepares each instance in
    place (`setActive(false)` -> `setupProcessing` -> `setActive(true)`, which is what every DAW
    does), so parameters survive because the component survives. `getState`/`setState` is still
    wanted for sessions, but it was never the right remedy for this.

24. **Bypass removes the plugin from the published view rather than asking it to bypass itself.**
    There is no latency to preserve -- sec. 3.7.1 makes the whole design zero-latency -- so
    skipping it is simpler than trusting each plugin's own bypass parameter to be implemented
    well. The instance stays in the rack, keeps its parameters, and keeps its editor alive.

25. **Mutations that touch an instance retract the chain first; mutations that only reorder do
    not.** Re-preparing writes to an object the audio thread may be inside, so `rebuild` publishes
    null and waits for quiescence before touching anything. Insert, move and bypass only change
    *which* instances a view names, so an ordinary atomic swap is enough. `removePlugin` publishes
    the view without the instance and only then destroys it -- and if the audio thread failed to
    let go within the grace period, the instance is *stranded* rather than freed. Stranded
    instances are reported by `Engine::strandedPlugins()` and released at teardown: leaking one is
    strictly better than freeing memory the audio thread is reading.

### 7.3 Parameter delivery and the UI

26. **An edit is applied to the half of the plugin that did not make it.** `ParameterEdit` now
    carries an `Origin`, stamped by `ComponentHandler::submit` from the thread it runs on -- which
    is the one thing the caller cannot supply. An `Audio`-origin edit means the processor moved the
    parameter itself, so the *controller* is told, and the editor follows. A `Control`-origin edit
    means the editor moved it, so the *processor* is told, through `inputParameterChanges`.

    The second direction is new and it is what makes a knob work at all: for a split
    component/controller plugin the two halves are separate objects forbidden to talk to each
    other, so without the host carrying the value across, turning a control changes nothing
    audible. Routing by origin rather than sending everything both ways is not tidiness -- pushing
    a value into the controller in the middle of the user's own gesture is how a knob ends up
    fighting the mouse.

27. **`inputParameterChanges` is written only by the audio thread, from a ring.** The plugin reads
    those queues during `process`, so the control thread cannot touch them; it pushes
    `(id, value)` onto an SPSC ring instead and `PluginInstance::process` drains a bounded number
    at the top of the next block. The bound is the number of queues `prepare` warmed, which is
    what keeps `addParameterData` inside preallocated storage.

    Every value is stamped at **sample offset 0**, and that is load-bearing rather than lazy: the
    SDK's `addPoint` *replaces* a point at an offset it already holds, so repeated values for one
    parameter within a block coalesce into one point. A dragged knob therefore costs one queue slot
    per parameter per block however fast it is dragged, and the point list never grows. Sample
    accuracy would be meaningless anyway -- the gesture came from a mouse, and protocol v1 has no
    transport to place it against.

28. **The GUI thread *is* the control thread, and `EngineHost` is where that is declared.** Both
    `engine::Engine` and `ipc::ValetSupervisor` document themselves as expecting one control
    thread and are not thread-safe against themselves; naming which thread that is, in one place,
    is what keeps every rack mutation and both servicing calls on it. The single thing that
    genuinely arrives from elsewhere -- the supervisor's state callback, on the supervisor's own
    thread -- is turned into a Qt signal and nothing else, so its delivery is queued onto the GUI
    thread and the engine is still only ever touched from one place.

29. **An editor is released before the engine is allowed to touch its plugin.** `EditorManager`
    holds that rule, because the engine cannot: an `IPlugView` holds the plugin's controller and
    its child HWND, and destroying the instance first leaves a live view that will dutifully call
    `removed()` on freed memory. The failure is not visible where it is caused. So `removePlugin`
    from the rack panel closes the editor first, and `~MainWindow` closes them all by hand rather
    than trusting Qt's child destruction -- Qt deletes children *after* member destruction, which
    would be after the engine is already gone.

    Editors are keyed by `PluginInstance*`, never by rack position. A position is not an identity:
    inserting before an open editor moves it. The instance does not move, because the rack owns it
    and outlives every published chain (item 23).

30. **Bypass does not touch the editor; removal does.** Bypassing only changes which instances the
    published view names, so the plugin keeps its parameters and its editor stays live and useful
    (item 24). Removal destroys the instance. Reordering is the bypass case: nothing is
    re-prepared, so nothing has to be closed.

31. **The rack panel keeps no model of its own.** Every button calls the engine and then rebuilds
    the whole list from what the engine now says. A few list items per click buys the guarantee
    that the two can never disagree, which is a bug class rather than a bug.

32. **Attaching is never automatic.** Attaching processes every sound on the endpoint,
    system-wide, and a stalled client holds up `audiodg.exe` for up to a second (sec. 3.7.1). The
    shell therefore starts detached, says in the window what attaching means, and only
    `--attach` on the command line does it without a click.

33. **The shell has a command line, for verification rather than for use.** A GUI whose only route
    into a given state is a sequence of mouse clicks cannot be checked without a person;
    `--vst3`, `--editors` and `--attach` are how the two-plugin, two-editor, attached state in
    section 4 was reached and screenshotted. It is `--vst3` and not `--plugin` for a reason worth
    knowing -- see section 8 item 15.

35. **The application icon lives on the executable, and nowhere else.** `ui/aip_ui.rc` attaches
    `ui/assets/mixing-table-png.ico` as resource id 1 (the shell shows the lowest-numbered icon
    resource, so the id is not arbitrary), and no Qt window icon is ever set. Explorer, the taskbar
    and Alt-Tab read the executable; the title bars are stripped in `window_chrome.cpp`.

    Removing a title-bar icon on Windows 11 takes four steps, not one, and getting three of them
    right leaves an icon or -- worse -- leaves the *slot*, so the caption sits indented past an
    empty square. `WS_EX_DLGMODALFRAME` is the only frame style that omits the slot rather than
    reserving it, but on its own it does nothing, because Windows falls through to the window class
    icon (Qt registers its classes with one extracted from the executable) and then derives a small
    icon from `ICON_BIG` if `ICON_SMALL` is empty. So all of it has to go: the style set, both class
    icons cleared, both window icon slots cleared, and `SWP_FRAMECHANGED` to make the frame
    recalculate. A transparent icon is *not* a substitute -- it hides the picture and keeps the
    indent.

    The cost is that the taskbar icon now arrives by fallback (window -> class -> executable) rather
    than being set explicitly. Setting `ICON_BIG` to keep it explicit is exactly what puts the icon
    back in the title bar.

    **The shell's caption text is blank as well**, which is what modern Windows applications do
    rather than repeating their own name back at the user. `setWindowTitle(QString())` does not
    achieve it on its own: Qt reads an empty widget title as "no preference" and substitutes
    `QCoreApplication::applicationName()` when it creates the native window, so the name appears
    anyway. `clearTitleText` clears the native window text afterwards, which is the only way to
    mean it -- and it only works for a window whose title never changes, because a later
    `setWindowTitle` hands control back to Qt.

    With a standard frame the caption text *is* the window title, so the taskbar and Alt-Tab labels
    are blank too; separating them would mean drawing the title bar ourselves. The icon is what
    identifies the application there instead. **Plugin editors keep their titles** -- with several
    open at once the plugin's name is the only thing telling them apart.

36. **`ui/` keeps its headers next to its sources**, unlike `protocol/`, `ipc/` and `engine/`, which
    all use `include/aip/<component>/`. Nothing links `ui/`; a public/private split with one side
    empty is just ceremony. `tools/editor_spike` set the precedent.

37. **One scanner child per *scan*, made resumable -- not one per plugin.** Sec. 7.2 says "one
    short-lived scanner process per scan", and per-plugin isolation seems to argue for one process
    per candidate. It does not have to: the child announces each bundle before it touches it, so a
    parent reading the record stream always knows what was on the table when the child stopped. A
    crash therefore costs one entry and one relaunch, and a clean machine costs exactly one
    process. Measured on this machine: 22 bundles, 3 of them fatal, 4 processes. One process per
    plugin would have been 22 with nothing gained.

38. **The wire format is one record per line, not JSON.** The child may stop mid-sentence at any
    instant -- that is its job. JSON cannot be read without its closing bracket, so a plugin that
    faults would make the *earlier* entries unreadable too; line records are complete the moment
    their newline lands. Values are escaped to printable ASCII (`\\` and `\xHH`) because plugin
    names, vendors and paths are third-party text under no obligation to be ASCII or even valid
    UTF-8, and a newline in one would end its record early. Note this is not sec. 6.6 spreading:
    that rule governs our files, this one governs a transport that must not fail on exactly the
    plugins the scanner exists to survive. Unknown keys are ignored, so the two ends can differ in
    version without either being taught about the other.

39. **Every abnormal child exit consumes exactly one entry.** If the child announced a bundle, that
    bundle is charged. If it died before announcing anything -- a missing DLL, a policy blocking
    the executable -- the *next* entry is charged anyway, even though it is innocent. This is not
    sloppiness, it is the loop's termination guarantee: a child that dies instantly would otherwise
    be relaunched against an unchanged work list for ever. Charging an innocent plugin is visible
    and recoverable; a scan that never ends is neither. A child that exits cleanly between entries
    having made progress is charged nothing, because progress is what rules out the loop.

40. **Records travel on a private inherited handle; the child's stdout goes to the null device.**
    Plugins print. A banner written to stdout during `initialize` would land in the middle of a
    record and corrupt the entries either side of it, so the record stream gets a pipe of its own
    and the plugin gets stdout to itself. Handles rather than stdio also mean there is no CRT
    buffer to flush and none to lose in a fault. The child is given an explicit
    `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` rather than blanket inheritance, because the shell holds
    the valet's shared-memory and event handles (sec. 4.2) and a process about to be killed for
    hanging has no business holding duplicates of them.

41. **The child suppresses every dialog Windows would put up for a crash.** `SetErrorMode`, an
    unhandled-exception filter that terminates, `_set_abort_behavior` and an invalid-parameter
    handler. Every one of those paths otherwise **blocks on user input**, which turns a crash the
    parent handles in milliseconds into a hang it can only resolve by timing out -- and, in an
    unattended scan, into a process sitting there indefinitely. This is not defensive tidiness: the
    one plugin that crashed on this machine exited via `abort` (code 3), so without
    `_set_abort_behavior` that entry would have cost 60 seconds instead of milliseconds.

42. **The parent looks for `aip_scan.exe` next to the running executable first**, and only then at
    the `AIP_SCAN_EXECUTABLE` compile definition. The first is what will be true once installed;
    the second exists because Ninja Multi-Config gives every target its own output directory, so in
    a build tree `aip_scan.exe` is not next to `aip_ui.exe`. Same escape hatch, and same reasoning,
    as `AIP_TEST_PLUGIN_PATH` in `tests/`. `locateChildExecutable` is public so the failure can be
    reported as itself: a missing scanner and a machine full of broken plugins otherwise produce
    the same report.

43. **The shell's plugin catalog is in memory only, and scans on the first Add rather than at
    start-up.** Asked for directly by the project owner on 2026-08-22: scan when the cache is empty
    and on demand, and defer persistence until real state persistence exists. So every launch is a
    fresh install as far as the shell is concerned, and the picker's Rescan button is the "on
    demand" half. Scanning at start-up was rejected for a reason worth keeping: a shell that spends
    two minutes scanning before it will draw is worse than one that scans when first asked for a
    plugin, and on this machine that is not a hypothetical two minutes. The catalog lives in
    `RackPanel` because that is the only panel that adds a plugin -- no singleton, no global.

44. **The scan's progress dialog pumps the event loop by hand rather than using a QThread.**
    `std::async` for the scan, a one-slot progress snapshot behind a mutex, and
    `QApplication::processEvents` on a 50 ms tick. Smaller than queued signals for one worker and
    one modal dialog, but the reason it is *right* rather than merely smaller is that
    `EngineHost`'s servicing tick is a `QTimer`: pumping the loop keeps it firing, so a chain goes
    on being serviced and a format change goes on being acted upon throughout a scan that may last
    minutes. Blocking the GUI thread outright would stall the control plane for the whole scan --
    which, since the GUI thread *is* the control thread (item 27), is not a UI nicety but a
    correctness property. Untested with audio flowing; see section 4.

45. **The scanner child runs in the temp directory, not the caller's.** Plugins write files into
    the current directory (trap 22), and a scan loads every plugin on the machine, so a scan run
    from any directory that matters would collect all of that litter at once. Nothing legitimate
    resolves against the current directory in a scanner child -- a plugin finds its own resources
    through its module path, which the SDK's loader supplies -- so this costs nothing. Note what it
    does *not* do: `ui/` still loads plugins in its own working directory, and the file that
    prompted this was written there rather than by a scan.

46. **One YAML file holds everything, and where it lives decides the mode.** Asked for directly by
    the project owner on 2026-08-22: a single text file, YAML, read from next to the executable
    first and from `%APPDATA%/audio-ipc2/` otherwise; written back to whichever file it was read
    from, and to AppData when there was nothing to read. So AppData is the default without being
    hard-coded as one, and a user opts into portable mode by *putting a file next to the exe* --
    even an empty one, which is why an empty file has to parse to an empty session rather than to
    an error. There is no mode flag anywhere, and nothing to keep in sync: the file's own location
    is the setting.

47. **`config/` is its own component and does not depend on Qt.** It could have lived in `ui/`,
    which is its only consumer. Keeping it out buys a seam the tests can reach: every assertion
    about the file format, the two locations and the capture/apply round trip runs without a
    window on screen, which is the difference between fourteen automated tests and a manual pass.
    It depends on `engine/` because a saved rack is a rack of real plugins, and on nothing else.

48. **A plugin's state is captured as two blobs, and the second one is skipped for a
    single-component plugin.** `IComponent::getState` is the one that matters -- it is what makes
    the audio come back the way it was left. `IEditController::getState` is the editor's own
    business and is captured too, but only when the component and controller are separate objects.
    The reason is a name clash: a single object implementing both interfaces has *one*
    `setState`, and the SDK itself works around this by macro-renaming the controller's pair to
    `set/getEditorState` while it includes the header. A plugin that did not take that workaround
    would have one function behind both vtable slots, and handing it the controller blob would be
    handing it the wrong data under a name it cannot distinguish. The cost is that a
    single-component plugin does not remember which editor page it had open. That is worth
    strictly less than not corrupting its processor state.

49. **State is applied between instantiation and `prepare`, which is why
    `insertPluginWithState` exists.** VST3 permits `setState` on an active component -- preset
    recall during playback is exactly that -- but a plugin is entitled to expect it before
    `setupProcessing`, and restoring into an instance that has already negotiated its busses is a
    needlessly hostile way to discover which plugins disagree. Every other insertion path returns
    with the instance already prepared, so this could not be a call the caller makes afterwards.
    `PluginInstance::loadState` asserts the ordering rather than trusting it.

50. **A plugin that refuses its state is still inserted, and the call still returns true.** The
    rack is what the user built; one unreadable blob -- which is what a plugin update looks like
    from here -- is not a reason to throw the rest of it away. `error` comes back non-empty on
    success in that one case, which is the only place in `Engine` where that is true, and it is
    how the shell says "your plugin is here but it starts from defaults" without calling it a
    failure. The same principle runs through `config::apply`: a missing plugin, a bad class id and
    a rejected blob each cost their own entry and nothing else.

51. **A session file that cannot be read is never written over.** If `readSession` fails, the
    shell blocks saving for the rest of the run and says so. The alternative is worse than it
    looks: a file that fails to parse leaves an empty rack, and saving on the way out would write
    that empty rack over the only copy of what the user is trying to recover. A hand-edit with a
    typo in it should cost a restart, not a rack. Writing goes through a temporary file and a
    rename for the same reason.

52. **The session is written on close and nowhere else.** No autosave, no write-on-change. A rack
    mutation costs a `getState` per plugin, and a plugin's `getState` is a real serialization --
    doing it on every click would put that work behind the mouse. The cost is that a crash loses
    whatever changed since the last clean exit, which is recorded in section 4 rather than
    designed around.

53. **The saved window geometry is checked against the screens that exist now.** A rectangle whose
    centre is on no current screen is discarded and the default size used instead. An external
    monitor that is not plugged in this time should not cost the user their window, and a window
    restored off-screen is invisible in a way that also hides the fact that it is invisible.
    `normalGeometry`, not `geometry`, is what gets saved: for a maximized window the second one is
    the screen, and restoring that would make un-maximizing do nothing.

54. **The cached scan report is validated per entry, not trusted or discarded wholesale.** Chosen
    by the project owner on 2026-08-22 over "trust until Rescan". On the first Add after a launch
    the standard locations are walked -- cheap, loads nothing -- each bundle is stamped from
    directory metadata, and only bundles whose stamp disagrees with the cache go to a child
    process. The requirement that decides it is item 43's, restated: a cache must never be the
    reason a plugin someone just installed is invisible. Trusting the cache until the user presses
    Rescan moves that failure from "every launch" to "until you remember", which is worse, because
    it is intermittent.

    Two details are load-bearing. A cached entry with **no** stamp is dropped rather than kept: an
    entry that cannot be verified would be believed forever, so an invalid stamp deliberately does
    not compare equal even to itself. And a bundle is stamped *after* it is probed, not before --
    a plugin caught mid-install would otherwise be recorded under the stamp it had while it was
    still being written, and never looked at again.

    Observed on this machine: one of the 22 installed bundles is a dangling symlink to a build
    output that no longer exists. It cannot be stamped, so it is re-probed at every start. That is
    the right outcome -- it costs a failed `LoadLibrary` and the picker greys it out either way.

55. **Being attached is part of the session, and is restored.** Asked for directly by the project
    owner on 2026-08-22, reversing what `config/` was first written to do. The argument that this
    replaces -- attaching takes over the machine's audio system-wide (sec. 3.7.1), so it must be a
    deliberate act -- was answering the wrong question: it is an argument for the *first* attach
    being asked for, and none at all for asking again every morning. Being attached is a state
    someone put the application into, which is the definition of what a session file holds.

    Two details keep it honest. The flag is written from `EngineHost::attached()` and not from the
    button, because a link can end without anyone pressing anything -- another client takes the
    stream (sec. 4.1), or the king goes away -- and a session that recorded "attached" after being
    displaced would reattach into a fight it already lost. And the reattach is suppressed when the
    endpoint the session named is no longer enumerated: taking over whatever device happens to be
    default now, because the one the user chose is unplugged, is not restoring their state, it is
    guessing at it with the whole machine's audio as the stake.

56. **A session that kills the shell costs one start, not the application.** Two mechanisms, in
    the order they are cheap. First the scan report: a module the catalog reports as anything but
    usable is never loaded, because a child process already died finding that out (sec. 7.2) and
    here it would take the shell instead. That covers every plugin a scan has met, at no cost.
    Second, a breadcrumb, for everything else -- the path about to be loaded is written to a
    sibling of the session file and flushed before the load, and cleared when the load returns.
    Still there at the next start means it names what stopped the last one.

    `scanner/` is the thorough answer and the wrong one here: probing every rack entry in a child
    at every start costs a process per plugin for knowledge the catalog usually already has.

    Three details. The breadcrumb is **consumed** when it is read, not merely read -- otherwise
    the entry it names could never be retried, because clearing `blocked` by hand would be undone
    by the same stale file at the next start, invisibly. A blocked entry is **kept in the file**,
    at its position, rather than dropped: it is still part of the chain the user built, and this
    way the reason is written where they can read it and clearing one flag is how they ask for a
    retry. And the policy lives in `config/` rather than `ui/`, because what is dangerous is a
    property of a session and a scan report and needs no window to be reasoned about or tested.

    What it does not cover is a plugin that crashes *after* loading, which is section 5 item 1.

57. **The portable folder is built by a dependency walk, not by a deployment tool.** Asked for by
    the project owner on 2026-08-22: a folder that can be copied to another machine and run.
    `file(GET_RUNTIME_DEPENDENCIES)` walks the import tables of both executables transitively and
    resolves each name against the pixi environment, which is the only mechanism that gets the
    conda-forge shape right -- Qt there is a dozen separate packages, and a Qt-aware tool ships Qt
    and leaves them behind (trap 24). What a dependency walk cannot see is a plugin, because
    nothing imports one: those are a list, and the list is a decision recorded in
    `package_impl.cmake` next to the reason for each entry.

    Three things go in that are not dependencies of anything. `aip_scan.exe`, because the scanner
    looks for its child beside the running executable (item 42) and a package without it reports
    every plugin on the machine as broken. A `qt.conf` pinning `Plugins = plugins`, because
    otherwise Qt falls back to the prefix compiled into `Qt6Core` -- the path this machine's
    environment happens to live at -- and a package tested here would load the developer's plugins
    and pass while the same folder failed everywhere else. And an `aip_config.yaml`, because a
    file next to the executable *is* portable mode (item 46): a folder you carry to another
    machine should keep its settings in itself, and this is how that is said.

58. **`--scan` exists so the catalog can be checked without a person clicking Add.** The command
    line is for verification rather than daily use (ui/src/main.cpp), and the catalog is the one
    part of the session whose cost is measured in minutes -- so "the cache is being used" had to
    be something a run could demonstrate rather than something the code asserts about itself. It
    does exactly what the first Add does and reports the summary through the log view.

59. **A plugin with no editor of its own gets one drawn from its parameter list.**
    `createView(kEditor)` returning null is a legal VST3 answer, not a failure, and a plugin that
    gives it was previously loadable, audible, and completely unadjustable -- the shell reported
    "the plugin has no editor view" and stopped. `GenericEditorWindow` draws a row per parameter
    instead: name, slider, and the value as the plugin spells it through `getParamStringByValue`.

    Three things about it are not obvious. **An edit made there has to reach both halves of the
    plugin**: a plugin's own editor sets its controller itself and reports through
    `IComponentHandler`, which item 26's drain carries to the processor, and an edit that starts
    in a window of *ours* has neither going for it -- hence `PluginInstance::setParameter`, which
    does both and is a separate path rather than a special case inside the drain. **The values are
    polled, not pushed**, because nothing tells a host that a plugin moved its own parameter; a
    100 ms timer re-reads the controller, skipping any slider the user is holding, since a poll
    that writes into a control mid-drag makes it stutter. And **read-only parameters are shown and
    disabled rather than omitted**, because for a meter or a gain-reduction readout the value is
    the entire point of it.

    `EditorManager` falls through to it on *any* failure to embed the plugin's own editor, not
    only on "no view": from the user's side a view we cannot host is in the same position as one
    that does not exist, and the reason is carried into the status line rather than discarded.
    Both kinds of window now share a `PluginEditorWindow` base, so item 29's release-before-destroy
    rule covers them by construction instead of by a second code path remembering to.

60. **The generic editor is sized from the layout's `sizeHint`, not from a row height times the
    parameter count.** The first version did the latter and was wrong by a third on the machine it
    was written on -- the window opened with a quarter of it blank. A guessed row height cannot
    survive a different font size or display scale, and the grid already knows exactly what its
    rows came out to. Past a share of the screen height the rows scroll instead, because a plugin
    with two hundred parameters is not a reason to produce a window taller than the desktop.

61. **The rack is prepared from the endpoint's configured format, before any block arrives.**
    Protocol v1 announces the format nowhere (sec. 4.5), so `serviceFormatChange` keys off an
    observed block and there is nothing to observe until someone plays something. A client that
    attached to a quiet endpoint therefore sat with every plugin `[not prepared]`, unwarmed, and
    with no way to learn that one of them refuses the format -- until the user happened to start
    audio, possibly hours later. `Engine::prepareSpeculatively` guesses from
    `PKEY_AudioEngine_DeviceFormat`, which we were already reading for the channel mask (item
    11b) and which carries `nSamplesPerSec` and `nChannels` too.

    The safety argument is that a wrong guess was already a handled case. `ChainProcessor`
    compares every block against the format its chain was built for and passes it through
    untouched on a mismatch; `serviceFormatChange` then rebuilds from what was actually observed.
    So guessing wrong costs one passed-through block and one rebuild -- exactly what a format
    change costs anyway -- and there is a test that drives that path deliberately.

    Two things it must not do, both tested. It must not touch `servicedFormatKey_`, because the
    first real block still has to be examined -- it is the only thing that can contradict the
    guess. And it must not overwrite a format a block already established, or re-attaching to the
    same endpoint would tear down a chain built from evidence and replace it with one built from
    a guess. `builtFormatIsSpeculative()` distinguishes the two claims, and the rack shows
    "(expected)" until a block confirms it.

62. **A plugin is run for four blocks the moment it is prepared, before any audio can reach it.**
    First-call behaviour -- allocating, building tables, faulting outright -- otherwise happens on
    the valet thread the first time the user plays something, which can be hours after they loaded
    the plugin and with nothing on screen connecting the two. `PluginInstance::warmUp` moves it to
    the moment they pressed the button. Called from the two places a plugin is prepared: inside
    `rebuild` (after `retract()`, so every instance is quiescent) and inside `insertPluginImpl`
    (before the instance joins the rack, so no published chain can name it). That precondition --
    no published chain names the instance -- is the whole safety argument; `process` is not safe
    to call beside the audio thread.

    Fed low-level deterministic noise, not silence, and the APO is the reason. It returns early on
    `BUFFER_SILENT` without publishing anything at all (`AudioIpcApo.cpp:270`), and plugins take
    the same shortcut internally -- so a block of zeroes would warm up only the plugins that had
    nothing to warm up. Run at `maxFrames` rather than a typical block size, because a plugin that
    sizes something lazily sizes it for what it is shown.

    This was chosen over the obvious alternative of opening our own WASAPI stream and playing an
    inaudible tone to make the endpoint go live. That would additionally get the *authentic*
    format at attach time, but it is the weakest of the three benefits -- a chain built from the
    device format and later contradicted just rebuilds, which is the designed path -- and it costs
    a relay click on every AV receiver and USB DAC that pops when a stream starts on an idle
    endpoint, plus a failure mode when another app holds the endpoint exclusively.

63. **The warm-up cannot tell you what a plugin allocated, and the report says so.**
    The first version claimed it could. The detector replaces `operator new` per *image*, and a
    VST3 plugin is a DLL carrying its own -- `rt/src/alloc_hooks.cpp` says exactly this in its
    header comment, about Qt and "later the VST3 plugins". So a plugin's allocations resolve to
    the CRT's operators and are invisible. The fixture allocates on its first process call on
    purpose and the probe still reports zero; there is a test that asserts that zero and explains
    it, so the claim cannot quietly come back.

    What survives is what mattered: making the allocation happen on the control thread instead of
    the valet thread needs no counter. A nonzero count in `WarmUpReport::violations` means *our*
    processing path misbehaved, and the shell reports it as a defect on our side rather than as a
    fact about the plugin.

64. **`rt::ViolationProbe` runs a scope as a real-time section but counts privately.**
    Introduced for the warm-up and useful beyond it. A plain `RealtimeGuard` would have charged
    four blocks of deliberate third-party execution against the process-wide counters, and sec.
    7.4.3's acceptance criterion is that those are *exactly zero* after steady state -- charging a
    warm-up against them would make the one number this project is most careful about mean
    something else. The probe diverts counting into a caller-owned tally for the duration of the
    scope and restores the previous destination on the way out, so it nests and suspends rather
    than latching. It also suppresses `setBreakOnViolation`: a violation that was asked for is not
    a bug, and trapping on it would make the probe unusable in an interactive session.

65. **A plugin that faults while processing costs the attach, not every future start.** The other
    half of item 56, and the half with no call to bracket. A fault inside `process` happens on the
    audio thread, seconds or hours after the rack was built, and because being attached is
    restored with the session the next start walks straight back into it -- taking the machine's
    audio with it every time round.

    Two mechanisms were possible and the project owner chose between them on 2026-08-22. The
    rejected one times the attach: treat "it survived N seconds" as proof and clear the mark then.
    It needs a threshold that is a guess about how fast a bad plugin faults, and a wrong guess
    either protects nothing or withholds protection from a slow crash. The chosen one has no
    threshold at all -- a run that ended badly while attached simply does not get an automatic
    attach on the next start. The shell comes up detached, says so, and waits to be asked.

    So `config::AttachGuard` writes a mark when the shell attaches and removes it when the shell
    detaches or exits, and `config::shouldReattach` is the one place that decides -- attached in
    the file, endpoint still present, previous run ended tidily. Policy in `config/` rather than
    `ui/` for the same reason as item 56: none of the three questions needs a window.

    **The false positive is the part worth reading.** The project owner asked for this and
    immediately named the failure mode it must not have: applications that announce a crash the
    morning after a perfectly ordinary Windows restart. The cause is always the same -- the mark
    is cleared only on the application's own quit path, and a shutdown does not go through it.
    Windows asks first, though. Every top-level window is sent `WM_QUERYENDSESSION`, and an
    application killed for answering too slowly is killed *after* that message. So the shell
    watches for it with a native event filter and clears the mark from inside the message, which
    is early enough even when it never gets to run another line (`ui/src/session_end_filter.h`).
    `WM_ENDSESSION` is handled too: true means the same news without the question first, false
    means the shutdown was called off and the mark goes back on, because the shell is still
    attached. A native filter rather than `QSessionManager` because it runs before Qt decides what
    to do with the message and does not depend on Qt's session management being configured in.

    Two ends stay outside all of this and are reported as unclean: losing power, and being killed
    from Task Manager. For the first that is arguably the truth -- the shell really was processing
    audio when it stopped -- and the cost either way is one press of Attach.

    The dialog is deliberate, not a log line. Everything else this shell reports goes to the log
    view; this is the one thing where the user has already lost their sound to a crash they did
    not see, and a start that is quiet *on purpose* has to say so where they are looking.

66. **The shell does not try to notice a plugin whose state points at something that has gone.**
    Not laziness: there is no way to. Established on 2026-08-22 against NeuralAmpModeler, whose
    state is a model file's absolute path (section 4). Restore it with the path broken and every
    signal a host can read says success -- `setState` accepts, the parameters come back, the rack
    builds, and asking the plugin for its state again returns the failed blob **byte for byte**,
    so even a round-trip comparison sees nothing wrong. The plugin knows perfectly well; it writes
    `(FAILED)` next to the model name in its own editor and tells the host nothing, because VST3
    gives it nowhere to tell the host anything.

    Which settles the design question the other way from how it was posed. The tempting mechanism
    -- read the state back after restoring and warn when it differs -- was measured before being
    written and detects exactly nothing here, while a plugin that legitimately re-serializes
    differently would trip it every time. So: no mechanism, and the limitation is written down in
    section 4 instead. What the user gets is the plugin's own editor, which is where a missing
    model is legible anyway.

    Worth remembering for the *portable* case (item 46), which is where this stops being
    hypothetical: a session file carried to another machine brings absolute paths for the plugin
    bundles, which the shell reports when they fail, and absolute paths *inside* plugin state,
    which it cannot.

67. **An editor window opens centred on the shell's, and its position is not remembered.**
    Asked for by the project owner on 2026-08-22. Two decisions in it, and the second is the one
    with content.

    Not remembering positions is the easy half: an editor belongs to its plugin's window, not to a
    spot on the desktop, and a saved position is wrong as soon as the shell is on another monitor
    or the plugin has changed its own size. So there is nothing in the session file about editors,
    and `ui/src/window_placement.h` computes the position instead.

    The hard half was where the arbitrary-looking placement came from, because Qt does centre a
    window on its transient parent and appeared to be doing so. It was centring the *wrong size*.
    Both editor windows force their native window into existence in their constructors --
    `hideTitleBarIcon` needs an HWND -- and at that point the widget is still 100 x 30, so what Qt
    centred was a placeholder. The real size arrives afterwards and grows out of that top-left
    corner. Measured: two editors of different sizes opened at the *same* top-left, 57 x 34 up and
    left of the shell's centre, which is exactly half a default-sized window.

    Which is also why placing once at open is not enough, and this is the part worth keeping. The
    project owner's own question was how to know when the editor is ready -- and the answer is that
    we do not need to know, because the position is recomputed for every size the plugin asks for.
    NeuralAmpModeler is the case that requires it: it reports 750 x 530 before being attached, and
    then asks for 937 x 655 from its own event loop, well after `EditorWindow::create` has
    returned. There is no VST3 signal for "finished settling" to wait for.

    That following stops the first time the user moves the window, and how it detects that is
    deliberate. The obvious mechanism -- remember the position we last asked for, and treat any
    other as the user's -- does not work: the platform reports geometry changes through the event
    queue, so a move event carrying a superseded position arrives after the move that superseded
    it, and the first straggler disarms the placement. What is tested instead is the invariant: is
    the window *still* centred, recomputed from scratch, clamping included. Nothing in that depends
    on what order anything arrived in. `placed_` exists for the same reason from the other side --
    before the first placement the window is not centred on anything, and the move that
    `hideTitleBarIcon` causes was disarming the feature before it had run once.

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
15. **`-plugin` is a reserved Qt option, and Qt eats it silently.** `QGuiApplication` consumes
    `-plugin`/`--plugin` out of `argv` before any `QCommandLineParser` runs, and tries to load its
    value as a *Qt* plugin. There is no diagnostic of any kind: the argument is simply absent from
    `QCoreApplication::arguments()`, so the parser reports zero values and the program behaves as
    though nothing had been asked of it -- while the flags either side of it work perfectly, which
    is what makes it look like anything but an option-name collision. Hence `--vst3`. The full
    reserved list is in the `QGuiApplication` class documentation; `-style`, `-platform`,
    `-geometry`, `-title` and `-session` are the other easy ones to collide with.
16. **Tell a plugin its content scale *before* asking its view size.** `IPlugView::getSize` answers
    in whatever scale the plugin currently assumes, so a host that calls `getSize` and then
    `IPlugViewContentScaleSupport::setContentScaleFactor` sizes its window from one answer while
    the plugin draws to another. The symptom is an editor rendered correctly but small, in the
    corner of a window one scale factor too big -- which reads as a DPI conversion bug and is not
    one. NeuralAmpModeler did this until the two calls were swapped; ZL Equalizer 2 did not, so one
    plugin is not enough to notice. `tools/editor_spike` had them the wrong way round too and has
    been corrected, so the spike and `ui/` do not disagree with each other.
17. **C++20 plus Ninja Multi-Config plus an OBJECT library made the Release configuration
    unbuildable.** Declaring C++20 makes CMake emit a module-scanning (dyndep) step per target per
    configuration. With `CMAKE_CROSS_CONFIGS=all`, the per-object modmap of an OBJECT library
    (`aip_rt_hooks`) is declared once per configuration with the same output path, and ninja
    refuses to load the build file at all: `alloc_hooks.cpp.obj.modmap is defined as an output
    multiple times`. RelWithDebInfo built fine throughout, so this stayed invisible for as long as
    nobody tried the `release` preset -- which was until 2026-08-21. The project uses no C++20
    modules, so `set(CMAKE_CXX_SCAN_FOR_MODULES OFF)` in the root `CMakeLists.txt` fixes it and
    removes ~74 pointless dyndep steps per build as a bonus.
18. **Pixi strips quotes from arguments it forwards to a task.** `pixi run ui --vst3 "C:/Program
    Files/..."` reaches the executable as separate words, so a path with spaces arrives as an
    option value plus a handful of stray positional arguments. Fine for `pixi run ui` with no
    arguments, which is what the task is for; for anything with a path in it, put the pixi
    environment's `Library/bin` on `PATH` and run `build/ui/RelWithDebInfo/aip_ui.exe` directly.
19. **`catch_discover_tests` defaults to POST_BUILD, which is wrong in a multi-config build.** It
    writes *one* test list naming the executable of whichever configuration was built last, and
    `ctest -C <the other one>` then runs that binary instead. Build Release, then run the
    RelWithDebInfo suite, and you get `100% tests passed out of 44` while the five tests that need
    `AIP_RT_CHECKS` skip -- correctly, because they are compiled out of the binary actually being
    run -- and a skipped test does not fail a ctest run. The sec. 7.4.3 acceptance criterion
    silently stops being checked and the summary still says green. Fixed by
    `DISCOVERY_MODE PRE_TEST`, which defers discovery to ctest time where `$<CONFIG>` is known.
    **When a suite passes, check the skip count, not just the pass line.** `ctest --preset dev`
    must skip *nothing*; `ctest --preset release` must skip exactly those five.
20. **A green ctest run is not evidence on its own.** This is the second time the same shape of bug
    has hit this project (trap 4 was the first: a leading dot in a Catch2 tag kept the soak test out
    of discovery entirely). Both times the suite reported success while the most important test in
    it did not run. Treat "how many tests ran, and did any skip" as part of the result.
21. **A GUI executable cannot tell you why it will not start.** `aip_ui` is `WIN32`, so it has no
    console: a missing Qt DLL, a missing platform plugin, or a `QCommandLineParser` error all
    produce a process that exits with a bare code or a window that never appears. Run it from a
    shell with the pixi environment active, and if it dies without saying anything, that is the
    first suspect rather than the code. (Everything the shell has to say once it is up is in the
    Counters group and the log view underneath it, which is why they exist.)
22. **A plugin writes files into the current directory, and the source-hygiene test finds them.**
    On 2026-08-22 a 176-byte binary `device_info.txt` naming this machine's GPU appeared in the
    repository root and turned `ctest` red on the sec. 6.6 ASCII check -- byte 0xEA at line 1,
    column 131 of a file nobody in this project wrote. It arrived while the shell was being clicked
    through, so the writer is a plugin loaded in-process by `aip_ui`, whose working directory is
    wherever it was started -- the repository root, under `pixi run ui`.

    Two things follow. First, **a red hygiene test is not necessarily your fault**; look at what
    the offending file actually is before hunting for an em dash you typed. Second, the scanner
    child is now given the temp directory as its working directory (item 45), because a scan loads
    *every* plugin on the machine and would otherwise collect the litter of all of them at once.
    The shell is not fixed the same way and still loads plugins in its own working directory:
    `--vst3` accepts a relative path, so changing it there is a decision with a visible
    consequence rather than a free one.

    **It is NeuralAmpModeler**, identified on 2026-08-22 after it turned `ctest` red a second
    time. Three runs of the shell, each in a throwaway working directory: empty rack, nothing
    written; NAM in the rack with no editor opened, `device_info.txt` appears; ZL Equalizer 2 in
    the rack, nothing written. So it is written when the plugin is *instantiated*, not when its
    editor opens or when audio starts -- which also explains why the earlier hunt through the
    GPU/CUDA plugins found nothing, and why a scan never reproduces it: the scanner child works in
    the temp directory (item 45), where the file lands unnoticed. The shell does not, which is the
    open decision in section 5 -- and it now has a concrete case behind it rather than an
    anonymous one.

23. **CMake 4 will not configure a dependency whose version floor is below 3.5, and the escape
    hatch has a second half.** The pixi environment ships CMake 4.4; yaml-cpp 0.8.0 -- the newest
    release, from 2023 -- declares `cmake_minimum_required(VERSION 3.4)`, and CMake 4 removed
    compatibility with anything below 3.5 outright. The configure fails inside the dependency
    before a line of it is read. `CMAKE_POLICY_VERSION_MINIMUM 3.5` is the documented fix and it
    works, but it then makes the subproject behave *as though it were written for 3.5* -- which
    means CMP0077 is OLD, which means `option()` goes back to **clearing any normal variable of
    the same name**. Every `set(YAML_CPP_BUILD_TOOLS OFF)` and its siblings are silently discarded
    and the tools, contrib and clang-format targets switch themselves back on. CMake says so, in a
    warning per variable that names the variable it just threw away and is easy to read as noise
    from someone else's project. `CMAKE_POLICY_DEFAULT_CMP0077 NEW` is the other half.
    `cmake/vst3sdk.cmake` needs neither -- the SDK's floor is 3.25 -- so the comment there saying
    CMP0077 comes for free is true of the SDK and not of dependencies in general.

24. **Two ways to package nothing at all.** Both cost a run that looks like it worked.

    `windeployqt6` is the canonical Qt deployment tool and it fails on a conda-forge Qt with
    `Unable to query qtpaths: Process failed to start`, then exits *successfully* having copied
    nothing. The conda layout has `qtpaths6.exe` where the tool looks for `qtpaths`, so
    `--qtpaths <prefix>/Library/bin/qtpaths6.exe` makes it run. It is still not enough: given that,
    it copies Qt's own DLLs, ICU and the plugins, and none of the separate conda packages Qt6Core
    and Qt6Gui import -- freetype, pcre2, zlib, libpng, zstd, double-conversion and the rest. That
    is what `file(GET_RUNTIME_DEPENDENCIES)` gets right and a Qt-aware tool does not, which is why
    `cmake/package.cmake` uses the dependency walk and lists the plugins by hand instead.

    The second is ours. `find_package(Qt6 ...)` in `ui/CMakeLists.txt` sets `QT6_INSTALL_PREFIX`
    in *that directory's* scope, so at the top level it is empty -- and a packaging script handed
    an empty prefix does not fail. It skips a plugin directory that "does not exist", finds no Qt
    to search, and reports `Qt6Core.dll` among the dependencies it expects *Windows* to provide.
    The package is written, the task succeeds, and the folder does not run. Read the unresolved
    list: a Qt DLL in it means the search directory was wrong, not that Windows has one.

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
