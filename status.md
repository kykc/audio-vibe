# Project status

**Updated:** 2026-08-21, after the UI milestone: the Qt 6 Widgets shell (`ui/`), parameter
delivery from a plugin's editor into its processor, and two real third-party plugin editors hosted
side by side while the chain processes system audio. Before that, the same day: the Engine
milestone, the first real third-party plugin (ZL Equalizer 2, which found and fixed a side-chain
bus defect), the sec. 5.1 editor-hosting spike, and the rack/chain split that makes chain
mutation non-destructive.

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

**Where we are:** the IPC foundation, the VST3 host and a working GUI are all finished and verified
against the real deployed APO. A real VST3 plugin chain runs inside the `audiodg.exe` block loop,
driven from a window, with the plugins' own editors on screen. Nothing of the plugin scanner
exists yet.

Prove the tree is healthy in one command:

```
pixi run test          # expect: 100% tests passed out of 44, ~4 s, and NOTHING skipped
```

**Read the skip count, not just the pass line.** `pixi run test` (RelWithDebInfo) must skip
nothing. The `release` configuration exists too and skips exactly five -- the ones that need the
sec. 7.4.6 violation detector, which is compiled out of Release by design:

```
pixi run -- cmake --build --preset release && pixi run -- ctest --preset release
```

Both configurations build and both suites pass as of 2026-08-21. See section 8 items 17, 19 and 20
for why that sentence is worth a check rather than an assumption -- twice now, this suite has
reported green while the most important test in it did not run.

Then, on a machine with the old APO installed, prove interop against real hardware:

```
pixi run ui                                 # the shell; pick an endpoint and press Attach
pixi run probe                              # pass-through, console
valet_probe --inspect --plugin <path.vst3>  # load and prepare only; never attaches
valet_probe --plugin <path.vst3> --seconds 30
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
| Scanner | `scanner/` -- out-of-process plugin probing | Not started. Next up; see section 5 |
| Installer | `installer/` -- WiX v7 | Not started |
| APO rewrite | `apo/` -- the rewritten APO | Deferred by design (sec. 7.3, sec. 8) |

"Done for a single linear chain, mutable while running" is deliberate: the engine holds an
ordered rack, publishes a view over it, and lets the rack be inserted into, removed from,
reordered and bypassed while audio flows -- without disturbing the plugins not being touched.
What it does *not* yet do is listed in section 5.

"Done for a first cut" for the UI means: it attaches, it drives the rack, it hosts editors, and it
puts every counter that matters on screen. What it has no notion of is a session (nothing is
saved), a plugin list that survives a crash (no scanner yet), or any control of its own -- the only
way to change a parameter is the plugin's own editor.

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
ui/         src/ only -- an executable, nothing links it. main (OLE apartment, command line),
            main_window (the shell: endpoint attach, counters, log), rack_panel (a direct view of
            the engine's rack; no second model), plugin_picker (installed .vst3s, from the SDK's
            own search paths), engine_host (names the GUI thread as *the* control thread; the
            100 ms servicing tick), editor_manager (which editors are open, and the guarantee
            none outlives its plugin), editor_window (one editor, embedded per sec. 5.1),
            plug_frame (IPlugFrame), window_chrome (title-bar icon removal, item 35)
            aip_ui.rc + assets/ -- the application icon, attached to the executable
cmake/      vst3sdk.cmake -- the pinned SDK, and every workaround sec. 6.3 calls for
tests/      44 Catch2 tests. harness/synthetic_king (the sec. 4.7 producer), harness/
            test_processors, harness/wait_for, fixtures/aip_test_plugin (a real VST3 plugin
            built from the SDK). conformance, engine, planar, protocol_layout,
            realtime_safety, source_hygiene, spsc_queue
tools/      valet_probe -- console client against the real APO, with --plugin and --inspect
            (--inspect loads, instantiates and prepares plugins and reports bus layout,
            parameter count, latency and split-vs-single, without attaching to anything;
            it is the shape `scanner/` needs, one process further out)
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
- Source tree is ASCII-only (sec. 6.6), enforced by a tree walk on every `ctest` run -- `ui/`
  included.

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

**Not** proven -- do not assume any of these:

- Audible processing on real hardware. Everything against the real APO has been at unity gain or
  a flat EQ, on purpose: a real change would alter the machine's actual audio output.
- **A parameter gesture on a *real* plugin.** The path is proven by test against the fixture and
  the counter for it is on screen, but nobody has yet dragged a control in ZL Equalizer 2's own
  editor and watched `parameters delivered` climb. That is a five-second manual check and it is the
  first thing to do next session: attach, open an editor, move something, read the Counters group.
  It was skipped deliberately rather than forgotten -- moving an EQ band changes the machine's real
  audio output, which is the same line everything else here has stayed on the safe side of.
- More than two third-party plugins. ZL Equalizer 2 and NeuralAmpModeler are proven end to end;
  they are one JUCE-wrapped and one iPlug2-wrapped plugin, and the population they stand for is not
  obviously well represented by two.
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

0. **Five seconds of manual checking, first.** Attach, open a plugin's editor, drag a control, and
   watch `parameters delivered` in the Counters group climb. The mechanism is tested against the
   fixture but no human has moved a real knob yet; see section 4. If it does *not* climb, the
   suspect is `ParameterEdit::Origin` -- a plugin whose editor edits arrive from a thread we
   classify as the audio thread would be routed the wrong way.
1. **`scanner/` -- out-of-process plugin probing (sec. 7.2).** A short-lived executable per scan
   that enumerates `Module::getModulePaths()`, loads each candidate, records class info, and
   reports it back; crash isolation is the whole point, so a plugin that faults must cost one
   scan entry, not the session. `valet_probe --inspect` is already most of the child process --
   promoting it means giving it a machine-readable output and a parent that survives its death.
   The shell wants it: `ui/src/plugin_picker.cpp` lists the paths the SDK finds but loads each one
   in-process when the user picks it, so a plugin that faults on load takes the session with it.
2. **Plugin state serialization**, and then a session file. `IComponent::getState` / `setState`.
   The shell makes the absence conspicuous -- every restart is an empty rack -- and it is the last
   thing standing between what exists and something usable daily. Note that this is *not* what
   makes a parameter survive a format change; the rack owning the instances is (item 23).

Engine work still outstanding, roughly in order of how much it will be missed:

- **Act on `restartComponent`.** At minimum honour `kParamValuesChanged` and
  `kLatencyChanged`; currently every flag is drained and discarded.
- Handle `IAudioProcessor::getLatencySamples` at least by reporting it, even though protocol v1
  cannot compensate for it.

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
- Remembering window geometry and the last endpoint. `QSettings`, once there is a session format to
  put it next to.
- The sec. 5.2 "EQ/plot widgets" the design document mentions for `ui/`. Nothing needs them yet;
  they belong with whatever the project's own processing turns out to be, not with plugin hosting.

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
