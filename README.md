# audio-ipc2

A Windows-only system-wide audio processor. A user-mode **APO** inside `audiodg.exe` intercepts
every audio block from the Windows audio engine and hands it, through shared memory, to a
userspace process that runs a **VST3 plugin chain** -- synchronously, on the audio engine's own
clock, with zero added latency.

Two halves, named after their roles in the IPC protocol:

- **King** -- the APO (`apo/`). Producer. Creates all the shared objects. Lives in `audiodg.exe`.
- **Valet** -- the userspace client (`ipc/`, `engine/`, `ui/`). Consumer. Opens what the king made.

This README is for a **developer working on the project**. End-user documentation and the
installer are deliberately out of scope: the installer's shape is not decided yet.

Three other documents, and none of them repeats another:

| Document | Contains |
|---|---|
| `design_doc.md` | The normative specification. Protocol v1 is frozen there. Read the relevant section before writing code |
| `AGENTS.md` | Hard rules -- real-time safety, the ASCII-only rule, the build traps |
| `status.md` | Where the project actually stands today, what is proven, what is not, and every trap already paid for |

**Read `status.md` first.** It is maintained so that a work session can be thrown away and a new
one started without losing the plot.

---

## Layout

```
protocol/   header-only C++20 -- protocol v1 and APO identity. No Windows headers, no dependencies
rt/         real-time violation detector, lock-free SPSC queue
ipc/        the valet: BufferValet, the promoted audio thread, endpoint enumeration
apo/        the king: the APO DLL. The one target built /MT
engine/     VST3 host -- module loading, the plugin rack, preallocated process data
config/     the session file (YAML), presets, the scan cache
scanner/    aip_scan.exe -- out-of-process plugin probing, so a hostile plugin costs a child
ui/         aip_ui.exe -- the Qt 6 Widgets shell
tools/      valet_probe, apo_host, apo_admin, editor_spike
tests/      Catch2, plus the conformance harness (a synthetic king and a valet driver)
cmake/      the two pinned third-party dependencies: the VST3 SDK and yaml-cpp
```

---

## 1. Build and test locally

### Prerequisites

- **Visual Studio 2022 or 2026** with the Desktop C++ workload, and Windows SDK 10.0.26100+.
  MSVC is not redistributable, so pixi *activates* a local install rather than providing one.
  ARM64 build tools are **not** needed -- ARM64 is deferred (design_doc.md sec. 11.5).
- **[pixi](https://pixi.sh)**, which brings everything else (CMake, Ninja, Qt 6, Catch2).
- **Long path support**, both halves. The VST3 SDK has ~120-character internal paths and a deep
  source root breaks the build in ways that do not point at their cause:

  ```
  HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled = 1
  git config --system core.longpaths true
  ```

  Keep the source tree shallow for the same reason.

### The four commands

```
pixi run configure    # CMake, Ninja Multi-Config
pixi run build        # RelWithDebInfo
pixi run test         # ctest
pixi run package      # build/package -- a portable folder needing no pixi, Qt or VS
```

**The first build downloads about 124 MB** (the VST3 SDK archive) and compiles five SDK
libraries. After that it is incremental.

**`RelWithDebInfo` is the only usable configuration**, and it is not a preference: conda-forge
`qt6-main` ships no debug libraries, so a `Debug` build cannot link. `RelWithDebInfo` is
`/MD -Zi` with full PDBs and is fully debuggable. `NDEBUG` is stripped from it, so `assert` and
the real-time violation detector are live in the configuration you develop in.

A `Release` configuration exists as well, and the difference matters when reading test results:

```
pixi run -- cmake --build --preset release
pixi run -- ctest --preset release
```

### Reading the test results

```
pixi run test    # expect: 100% tests passed out of 153, and NOTHING skipped
```

**Read the skip count, not just the pass line.** RelWithDebInfo must skip nothing. `Release`
skips exactly eight -- the ones needing the violation detector, which is compiled out of Release
by design. Twice in this project's history the suite has reported green while the most important
test in it did not run; `status.md` sec. 8 items 17, 19 and 20 explain how.

**Everything must go through `pixi run`.** Catch2 is a conda-forge shared library and its DLL is
only on `PATH` inside the environment. A bare `aip_tests.exe` exits with a status code and no
output, which looks like a crash and is not one.

### Two environments

`pixi run <task>` uses VS2022; `pixi run -e vs2026 <task>` uses VS2026. They differ only in which
Visual Studio the activation package finds. On a machine with both installed, **delete `build/`
when switching** -- both configure the same tree and CMake caches the compiler path it resolved
first.

### Before you commit

Source and documentation are **ASCII-only** (design_doc.md sec. 6.6) -- `--` not an em dash,
`->` not an arrow, `sec. 4.3` not a section sign. This is enforced by a tree walk on every
`ctest` run and reports file, line, column and byte. It is not a style preference: a section sign
in a Catch2 test name silently breaks the `ctest` name-to-filter round trip.

---

## 2. Run and debug locally

### The fast loop: no APO, no audiodg, no sound card

Most work needs none of the audio stack. `apo_host` plays the audio engine and `valet_probe`
plays the client, and between them they exercise the entire protocol:

```
# terminal 1 -- the king (elevated; it creates Global\ kernel objects)
apo_host --signal sine:1000:-12 --seconds 60

# terminal 2 -- the valet
valet_probe --endpoint-guid "{A1B0DE11-2222-3333-4444-555566667777}" --gain 0.5
```

`apo_host` prints what it measures on the way out, so a gain applied by the client shows up as a
level change on the king's side. That is the whole loop, with the operating system removed.

### Registering the APO

Two separate acts, deliberately, because they have completely different blast radii.

**One: make the class loadable.** This writes two registry keys and touches no audio device.

```
regsvr32 aip_apo.dll        # elevated
regsvr32 /u aip_apo.dll     # removes exactly what the above added, and nothing else
```

Copy the DLL somewhere stable first (`C:\aip\` works). `audiodg.exe` holds it open once loaded,
which would block rebuilds if you registered it out of the build tree.

**Two: put it in an endpoint's effect chain.** This can silence the machine, so it is a separate
tool that takes a `.reg` backup of the whole render tree before every mutation.

```
apo_admin --list                                     # read-only, needs no elevation
apo_admin --install --endpoint 0 --restart-audio --yes
apo_admin --uninstall --restart-audio --yes          # puts back exactly what was there
apo_admin --install --legacy --endpoint 0 --yes      # the deployed 2013 APO, for A/B comparison
```

Then play something and attach:

```
valet_probe --endpoint 0 --gain 0.5
```

**Four things that will otherwise cost you an afternoon.** Each is written up in `status.md`
sec. 8; they are summarised here because they all present as "the APO does nothing".

1. **`--restart-audio` is not optional after an install.** The audio engine caches the endpoint's
   effect configuration and does not re-read it per stream. Without the restart the APO is never
   asked for, even if `audiodg.exe` is created fresh afterwards.
2. **A render endpoint materialises on first playback.** After a service restart, both
   `Get-PnpDevice -Class AudioEndpoint` and `valet_probe --list` can report nothing while the
   device is perfectly healthy -- Windows initialises endpoints lazily. Play a sound before
   concluding anything: `[System.Console]::Beep(1000,500)` is enough.
3. **Never stop `AudioEndpointBuilder`.** Only `Audiosrv` matters here; it owns `audiodg.exe`.
   Stopping the other one has cost this project a render endpoint that then came back under a
   different GUID, which silently invalidates a session file's saved endpoint.
4. **A populated modern effect slot makes the GFX slot dead letter.** `,5`/`,6`/`,7` and `,2` are
   strictly either/or. `apo_admin --install` clears the modern slots and saves them; `--list`
   shows them, with a note, when they are set.

If something is still wrong, turn on tracing before guessing.

### Traces from the APO

The APO lives inside a protected system process that cannot be attached to casually, so it can
tell you what it is doing. **Off by default**, enabled by one registry DWORD:

```
HKLM\SOFTWARE\Automatl\AudioIpc
    Trace  (DWORD)   0 or absent = off      (the shipping default)
                     1 = OutputDebugString  (DebugView, or an attached debugger)
                     2 = a log file at C:\Windows\Temp\aip_apo.log
                     3 = both
```

`2` is usually what you want: catching `OutputDebugString` needs a debugger attached to a
protected process, and `1` and `3` make it a system-wide serialising call on a machine that is
supposed to be doing real-time audio. The path is fixed because `audiodg.exe` runs as
`LOCAL SERVICE` and anywhere under a user profile would be unwritable.

The value is read per activation, so a change takes effect on the next stream -- no rebuild, no
re-registration. A healthy activation looks like this:

```
[21:07:46.514 aip_apo 11828] DllGetClassObject: handed out a factory, hr=0x00000000
[21:07:46.514 aip_apo 11828] CreateInstance: outer=non-null (aggregating)
[21:07:46.516 aip_apo 11828] Initialize: entered, cbDataSize=56
[21:07:46.516 aip_apo 11828] Initialize: endpoint objects at Global\TOMATL.AUDIO.IPC.{...}
[21:07:46.521 aip_apo 11828] LockForProcess: stream open at 48000 Hz x2 ch
[21:07:46.770 aip_apo 11828] UnlockForProcess: 27 blocks, 0 evictions
```

**Nothing is ever traced from the processing thread.** Every call site is control-plane --
`DllGetClassObject`, `CreateInstance`, `Initialize`, `LockForProcess`, `UnlockForProcess` and the
registration entry points. `APOProcess` has none, and there is deliberately no real-time-safe
trace variant, because one would get used. What the audio thread has instead is counters, printed
once from `UnlockForProcess`.

The other settings value, while you are in that key:

```
    ForwardSilentBlocks  (DWORD)   0 = default: a BUFFER_SILENT block is not published to the
                                       valet at all, matching the deployed APO
                                   1 = publish it, so plugin tails and meters keep running
                                       through silence -- at the cost of the full rendezvous and
                                       the whole plugin chain on every idle block
```

### Debugging inside `audiodg.exe`

`audiodg.exe` is a protected process. To attach a debugger, set
`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio` -> `DisableProtectedAudioDG` (DWORD) to 1
and restart `Audiosrv`. On a machine where that is not acceptable, the trace file is the
substitute and is what found the last real bug.

---

## 3. The command-line tools

Where the binaries land, all under `build/`:

```
tools/valet_probe/RelWithDebInfo/valet_probe.exe
tools/apo_host/RelWithDebInfo/apo_host.exe
tools/apo_admin/RelWithDebInfo/apo_admin.exe
scanner/RelWithDebInfo/aip_scan.exe
ui/RelWithDebInfo/aip_ui.exe
apo/RelWithDebInfo/aip_apo.dll
```

`pixi run package` produces `build/package`, which contains **only `aip_ui.exe` and
`aip_scan.exe`** plus their Qt dependencies -- it is the portable shell, not a developer kit. The
APO and the three tools here are not in it, and the installer that would carry them does not
exist yet.

### `valet_probe` -- the client, without the GUI

Attaches as a protocol v1 valet and reports what it sees. This is the quickest way to answer "is
the APO working".

```
valet_probe [--list] [--endpoint N] [--endpoint-guid GUID] [--gain G]
            [--plugin PATH]... [--inspect] [--scan] [--seconds S]
```

| Option | Meaning |
|---|---|
| `--list` | list active render endpoints, with the object names derived from each, and exit |
| `--endpoint N` | attach to the Nth endpoint from `--list` (default: the default endpoint) |
| `--endpoint-guid G` | attach by GUID, real device or not -- how you reach a king run by `apo_host` |
| `--gain 0.5` | apply a gain instead of passing through. Mutually exclusive with `--plugin` |
| `--plugin PATH` | run a real VST3 chain instead of a gain. Repeatable |
| `--inspect` | load and prepare the plugins, report their busses, and exit. Never attaches |
| `--scan` | probe every installed plugin out of process, report, exit. No APO involved |
| `--seconds S` | run for a fixed time instead of until Ctrl+C |

It prints a line a second: `blocks N (+~101/s) timeouts 0 malformed 0 reclaims 0 48000 Hz x2 ch,
480 frames`, and an audio-thread violation summary at the end that must read zero.

`--inspect` and `--scan` differ in where the plugin is loaded. `--inspect` loads it *in the
probe*, so a plugin that faults takes the probe with it; `--scan` drives `scanner/` and loads it
in a child. Use `--scan` on anything you have no reason to trust.

**No blocks arriving is usually not a bug.** `audiodg.exe` only creates the shared objects while
a stream is active, so with nothing playing you get `Attach cycles: 0`. Check playback first.

### `apo_host` -- drive an APO DLL with the audio engine out of the loop

The APO's equivalent of `scanner/`: it loads an APO DLL, hands it a fabricated
`APOInitSystemEffects`, locks it for a real `IAudioMediaType`, and pumps blocks at the endpoint
clock. **Needs elevation**, because it creates the `Global\` kernel objects.

```
apo_host [--dll PATH] [--endpoint GUID] [--signal SPEC] [--rate N] [--channels N]
         [--frames N] [--seconds S] [--silent-blocks] [--no-inplace] [--verify]
         [--list-signals]
```

| Option | Default | Meaning |
|---|---|---|
| `--dll PATH` | `aip_apo.dll` beside the exe | the APO to load. Point it at `C:\Windows\system32\AudioIpcApo.dll` to drive the deployed 2013 binary |
| `--endpoint GUID` | a synthetic one | the endpoint GUID the APO is told it belongs to; determines the object names |
| `--signal SPEC` | `silence` | the test signal fed in. See below |
| `--rate N` | 48000 | sample rate |
| `--channels N` | 2 | channel count |
| `--frames N` | 480 | frames per block (10 ms at 48 kHz) |
| `--seconds S` | 10 | how long to run. `0` runs until Ctrl-C |
| `--silent-blocks` | off | mark every block `BUFFER_SILENT` instead of `BUFFER_VALID` |
| `--no-inplace` | off | give the APO separate input and output buffers. The engine does not |
| `--verify` | off | check that output matches input, block by block |
| `--list-signals` | | print the signal bank and exit |

Test signals. **Levels are peak dBFS for every generator and must be below zero:**

```
silence                      digital black -- the baseline a level check is measured against
noise:<peak dBFS>            deterministic uniform white noise, e.g. noise:-20
sine:<Hz>[:<peak dBFS>]      continuous tone, phase-continuous across blocks, e.g. sine:1000:-12
```

Validated on the way in: the level must be negative, and a tone must sit between 20 Hz and
Nyquist for the chosen rate -- above it you would get a clean tone at the wrong frequency and no
warning. Adding a generator is one table entry in `tools/apo_host/signal_source.cpp`; the CLI,
`--list-signals` and the error messages all come off that table.

Output is a line a second with input and output levels side by side, so a client applying a gain
on the other end is directly visible:

```
blocks 900  in -12.0 dBFS peak / -15.0 rms   out -18.0 dBFS peak / -21.0 rms   worst block 0.123 ms
```

**Using a real endpoint's GUID is a trap when something is playing to it**: the deployed APO is
then publishing to the same object names and the valet quietly serves two kings, which looks like
a doubled block rate and nothing else.

### `apo_admin` -- endpoint effect chains, and the way back

The only tool that changes what runs on a real device. Every mutation writes a timestamped `.reg`
backup of the whole render tree to `C:\aip-backup\` first; that is not optional and there is no
flag to skip it.

```
apo_admin [--list] [--install [--legacy]] [--uninstall] [--endpoint GUID|N]
          [--backup-dir PATH] [--restart-audio] [--yes]
```

| Option | Meaning |
|---|---|
| `--list` | every endpoint, what is in its GFX slot, and whether a modern slot is shadowing it. Read-only, no elevation |
| `--install` | write our CLSID to the GFX slot, saving what was there to `OriginalGfxApo`, and clear the modern slots |
| `--install --legacy` | the same with the deployed 2013 CLSID -- how you switch a machine back and forth to compare |
| `--uninstall` | restore whatever was there before, modern slots included |
| `--endpoint GUID\|N` | act on one endpoint. Default: all of them |
| `--backup-dir PATH` | where the `.reg` goes. Default `C:\aip-backup` |
| `--restart-audio` | restart `Audiosrv` afterwards, so the change takes effect now |
| `--yes` | skip the confirmation prompt |

To undo anything by hand, with no build tree and no tooling: `reg import <the backup file>`, then
restart `Audiosrv`.

### `aip_scan` -- the plugin probe child

Normally driven by the shell or by `valet_probe --scan`, but runnable by hand on one suspect
bundle, where it prints its own wire format:

```
aip_scan [--report-handle N] [--no-prepare] [--no-editor] [--rate N] [--channels N] [<path>...]
```

With no paths it reads the bundle list from stdin, one escaped path per line.

---

## 4. The shell (`aip_ui`)

A Qt 6 Widgets application. One process hosts the GUI, the plugins, their editors and the valet
thread -- VST3 editors need a UI thread in the same process as their HWND, so splitting them
would buy nothing.

```
aip_ui [--vst3 PATH]... [--editors] [--attach] [--config PATH] [--scan]
```

| Option | Meaning |
|---|---|
| `--vst3 PATH` | load into the rack at startup. Repeatable. **Appends to the restored session** |
| `--editors` | open each loaded plugin's editor |
| `--attach` | attach to the default render endpoint. The only option that touches the machine's audio |
| `--config PATH` | use this session file instead of the usual two, and the way out of a session that will not load |
| `--scan` | bring the plugin catalog up to date and report |

Two naming traps, both paid for already: it is **`--vst3` and not `--plugin`**, and **`--config`
and not `--session`**, because Qt reserves `-plugin` and `-session` and eats them out of `argv`
with no diagnostic. Also, pixi strips quotes from forwarded arguments, so for a path with spaces
run the executable directly rather than through `pixi run ui`.

### What is on screen

Three groups, top to bottom.

**Link.** An endpoint combo box, `Refresh`, and `Attach`. Endpoints without this project's APO
registered are greyed out, sorted to the bottom, and cannot be selected -- attaching to one can
never work, because there is no other side to the rendezvous, and it looks exactly like a device
nobody is playing to. Hover for the reason.

**Rack.** The plugin chain, in order. Each row's **check box is the bypass control for that
plugin** and reads the way a rack reads: ticked is in the chain, cleared is bypassed. **Drag a row
to reorder the chain**; there are no move buttons. Buttons act on the selected plugin -- `Add...`
(goes through the out-of-process scanner, so a hostile plugin cannot kill the shell), `Editor`,
`Remove` -- and, set apart below, three that act on the whole chain: `Bypass`, `Save Preset` and
`Load Preset`. The rack can be mutated while audio is flowing without disturbing the plugins you
are not touching.

An `Editor` opens the plugin's own editor, or a control per parameter for a plugin that has no
editor to show. Editor windows are deliberately plainer than the shell: no title-bar icon, and no
minimize or maximize button -- an editor belongs to its plugin, and its caption says which one.
They stay resizable where the plugin's view is.

`Bypass` takes the **whole chain** out of the signal path: the endpoint's audio is handed straight
back, bit for bit, and nothing in the rack runs. It stays pressed until you release it, and while
it is pressed the link line says `chain bypassed`. Nothing is unloaded and nothing is
re-prepared -- every plugin keeps its settings and its editor, so switching back is immediate --
and the state is saved, with the session and with a preset, because a chain that was switched out
of the path is how you left it.

**Counters.** The numbers that say whether the thing is healthy, on two lines:

```
blocks N (101/s)   timeouts 0   malformed 0   reclaims 0   48000 Hz x2 ch
Audio-thread allocations 0   frees 0   locks 0
process: resident 90.6 MiB   peak 93.8 MiB   up 0:00:25
```

- `timeouts` climbing means the king is not publishing -- usually nothing is playing.
- `malformed` should be zero always; anything else means somebody is writing nonsense into the
  shared header, which any process on the machine can do (design_doc.md sec. 3.7.2).
- `reclaims` non-zero means the king evicted us for missing its deadline and we re-claimed.
  Recoverable, but worth noting.
- **The allocation counters must read exactly zero in steady state.** That is the sec. 7.4.3
  acceptance criterion, not a diagnostic, and it is compiled out of Release -- so read it in
  RelWithDebInfo. Resident set beside it is the other half: flat RSS over a long run.

Below that is a log view of what the shell has done.

The session -- the rack, each plugin's own state, whether the chain was bypassed, the cached scan
report, the window geometry and the last endpoint -- is one YAML file, written next to the executable if that directory is
writable and in AppData otherwise. It is saved on close *and* on `WM_QUERYENDSESSION`, so a
Windows restart with the shell open does not throw it away.

---

## Hard rules, in one paragraph

**The audio processing thread is real-time safe, without exception.** No heap activity, no locks,
no I/O, no COM, no exceptions, no unbounded loops, no first-touch page faults -- in our code, on
both sides. The single sanctioned exception is the KING/VALET event rendezvous, because that
rendezvous *is* the protocol. **Protocol v1 is frozen**: the client must interoperate with the
deployed APO binary, so deviations are defects rather than improvements, and improvements belong
to a future v2. Read design_doc.md sec. 4 and sec. 7.4 in full before touching `protocol/`,
`ipc/`, `apo/`, or anything the audio thread calls.
