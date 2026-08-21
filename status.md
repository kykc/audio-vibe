# Project status

**Updated:** 2026-08-21, at commit `7cac47d` ("First valet-side implementation").

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

**Where we are:** the client's IPC foundation is finished and verified against the real deployed
APO. Nothing of the VST3 host, the plugin scanner, or the UI exists yet.

Prove the tree is healthy in one command:

```
pixi run test          # expect: 100% tests passed out of 27, ~2 s
```

Then, on a machine with the old APO installed, prove interop against real hardware:

```
pixi run probe         # or: build/tools/valet_probe/RelWithDebInfo/valet_probe.exe --seconds 5
```

Expect a line per second reading `blocks N (+~101/s) timeouts 0 malformed 0 reclaims 0
48000 Hz x2 ch, 480 frames`. Zero blocks means the APO is not registered for that endpoint or
nothing is playing -- not necessarily a client bug.

**Everything must run through `pixi run`.** Catch2 is a conda-forge shared library and its DLL is
only on `PATH` inside the pixi environment. A bare `aip_tests.exe` exits with a bare status code
and no output, which looks like a crash and is not one.

---

## 2. Milestone map

Note on numbering: `design_doc.md` sec. 1 numbers its own *document* stages -- Stage 0 analysis,
Stage 1 stack decision, Stage 1.5 toolchain -- and all three are complete. Those are decisions,
not code. The milestones below are the *implementation* track and are deliberately named after
components rather than numbered, so the two schemes cannot be confused.

| Milestone | Scope | State |
|---|---|---|
| Design | Analysis, stack, toolchain, protocol v1 frozen | Done: `design_doc.md` sec. 2-6, and sec. 4 is frozen |
| **IPC foundation** | **`protocol/`, `rt/`, `ipc/`, conformance harness, probe tool** | **Done and verified** |
| Engine | `engine/` -- VST3 host: module loading, plugin chain, state, processing graph | Not started. Next up; see section 5 |
| Scanner | `scanner/` -- out-of-process plugin probing | Not started |
| UI | `ui/` -- Qt 6 Widgets shell, plugin rack, editor hosting | Not started |
| Installer | `installer/` -- WiX v7 | Not started |
| APO rewrite | `apo/` -- the rewritten APO | Deferred by design (sec. 7.3, sec. 8) |

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
tests/      27 Catch2 tests. harness/synthetic_king (the sec. 4.7 producer), harness/
            test_processors, harness/wait_for. conformance, planar, protocol_layout,
            realtime_safety, source_hygiene, spsc_queue
tools/      valet_probe -- console client against the real APO
```

Thread split, which is the load-bearing structural decision:

- **Control thread** (`ValetSupervisor`): opens handles, generates the valet id, claims the
  stream, faults in every page, retries, tears down. Allowed to allocate, log and lock.
- **Audio thread** (`ValetThread`): the sec. 4.4 rendezvous and one virtual call into a
  `BlockProcessor`. Nothing else. Real-time safe under sec. 7.4.1.

When `engine/` arrives it plugs in as a `BlockProcessor` implementation. The chain itself gets
built on the control thread and published by an atomic pointer store (sec. 7.4.3) -- the audio
thread must never do more than read that pointer.

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
- The sec. 7.4.3 acceptance criterion: **exactly zero** audio-thread allocations, frees and lock
  acquisitions over 20,000 blocks, and flat resident set. The detector has its own self-tests,
  so a zero reading means the instrument is live rather than absent.
- MMCSS "Pro Audio" promotion succeeds on the valet thread (sec. 4.6).
- Source tree is ASCII-only (sec. 6.6), enforced by a tree walk on every `ctest` run.

Proven against the real deployed APO on the development machine:

- The client attaches as a v1 valet and processes real blocks out of `audiodg.exe` at
  48 kHz / 2 ch / 480 frames, with zero timeouts, zero malformed headers and zero reclaims.
  Two runs, 304 and 202 blocks. This is what confirms the object names, the endpoint GUID form
  and the planar layout against the actual producer rather than against our own harness.

**Not** proven -- do not assume any of these:

- Audible processing on real hardware. The probe has only ever been run at unity gain, on
  purpose: a non-unity gain would alter the machine's actual audio output.
- More than one endpoint at a time, and endpoint switching while attached.
- A real format change (sample rate or channel count) driven by Windows rather than by the
  harness.
- Takeover between two real client processes. The `Stolen` path is covered only by the harness
  forging a foreign valet id.
- A long soak (hours) against the real APO. The zero-allocation soak runs against the synthetic
  king.
- ARM64. Declared supported in sec. 6.1, never built.
- Anything at all involving VST3, plugins, or a UI.

---

## 5. Next actions, in order

1. **Pin the VST3 SDK archive `URL_HASH`** -- open item sec. 11.1, and the only thing blocking
   the Engine milestone. Discover the current URL from the 302 redirect at
   `https://www.steinberg.net/vst3sdk`, download, hash, and pin it in a `FetchContent_Declare`
   with `SOURCE_SUBDIR vst3sdk`. Read sec. 6.3 in full first: every one of those five traps is
   real and none of them fails in a way that points at its cause.
2. **Stand up `engine/` as a `BlockProcessor`.** Smallest useful increment: load one VST3 module,
   instantiate one component, `setupProcessing` / `setActive` on the control thread, publish it
   by atomic pointer store, and run it from the existing valet thread. The soak test should stay
   at exactly zero audio-thread allocations across that -- if it does not, the work was not
   pushed far enough upstream (sec. 7.4.3).
3. **Preallocate the VST3 `ProcessData` / `AudioBusBuffers` / parameter-change structures once at
   stream open** (sec. 7.4.5), and make `IComponentHandler::performEdit` enqueue through
   `rt::SpscQueue` and return. That queue exists and is tested but is not yet used by anything.
4. Then `scanner/`, then `ui/`.

Worth doing at some point, none of it blocking:

- Add `clang-format` to `pixi.toml` with a `format` task. It is listed as project hygiene in
  sec. 6.1 but is not in the toolchain, so the 100-column limit is currently hand-maintained.
- Exercise the probe against a second endpoint and across a Windows-driven format change.
- Decide whether `ValetSupervisor` should expose endpoint hot-swap, which the UI will want.

---

## 6. Live blockers and open decisions

| # | Item | Owner | Blocks | State |
|---|---|---|---|---|
| 1 | Pin the VST3 SDK archive `URL_HASH` (sec. 11.1) | implementation | Engine | Open. Not yet attempted; needs a 246 MB download. |
| 2 | Minimum OS floor (sec. 8.1, leaning Windows 11 only) | project owner | APO rewrite | Open, blocks nothing yet |
| 3 | GFX vs modern registration slots (sec. 8.2, leaning GFX `,2` only) | project owner | APO rewrite | Open, blocks nothing yet |
| 4 | Staged porting plan (sec. 11.4) | project owner | -- | Open |

Nothing is currently blocking client work.

---

## 7. Implementation decisions not in `design_doc.md`

These were taken while building the IPC foundation. They are additive or interpretive, not
deviations from the frozen protocol, but a fresh session would otherwise have to re-derive
them.

1. **`rt/` is a component that sec. 7.1 does not list.** The violation detector needs a real
   translation unit (it replaces global `operator new`/`operator delete`) and the SPSC queue is
   shared by `ipc/` and, later, `engine/`. `aip_rt_hooks` is an **OBJECT** library, not static,
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

---

## 8. Traps already paid for

`design_doc.md` sec. 6.3 lists the five build traps found during toolchain verification. These
are the ones found while implementing the IPC foundation. Re-discovering any is wasted time.

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
   matched the APO. Caught only because MSVC warns C4129 on the resulting invalid escape. When
   writing a wide string literal containing a backslash, verify the bytes on disk afterwards.
   This is one of the reasons for the ASCII-only rule (sec. 6.6).
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

---

## 9. Keeping this file useful

Update it at the end of any session that changes the answer to "where do we stand". Specifically:

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
- Refresh the commit and date at the top.

Keep it ASCII-only (sec. 6.6) -- `pixi run test` checks this file too.
