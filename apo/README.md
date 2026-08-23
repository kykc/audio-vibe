# `apo/` -- the rewritten APO

A post-mix (GFX) system-effect APO that hands every audio block to a userspace valet over
protocol v1 and puts back what comes home. It is a **drop-in replacement** for the deployed 2013
binary (`AudioIpcApo.dll`, design_doc.md sec. 2): same object names, same header layout, same
rendezvous, same 1000 ms timeout, so the existing client attaches to it without knowing which one
it is talking to.

```
apo/
  include/aip/apo/
    buffer_king.h     protocol v1 king side -- the producer half (sec. 4.4, sec. 4.5)
    settings.h        the two registry knobs, read once per stream
  src/
    audio_ipc_apo.h   the APO class; the threading contract is documented at the top
    buffer_king.cpp
    dll_main.cpp      the four COM entry points, the class factory, self-registration
    registry.cpp      the handful of writes DllRegisterServer makes
    settings.cpp
    trace.h           control-plane logging, off by default
    aip_apo.def
```

Identity lives in `protocol/apo_identity.h`, which is the one place the APO and the client can
meet: the APO registers under `kApoClsid` and the client's `ipc/apo_registration.cpp` recognises
it there. CLSID `{C6A6A861-A99F-4F00-B636-657F38F353E9}` -- one hex digit from the legacy one, so
the two sort together in a registry dump and are never confused by anything that parses them.

## Using it

```
regsvr32 aip_apo.dll         make the class loadable (COM + the APO catalogue)
regsvr32 /u aip_apo.dll      undo exactly that, and nothing else

apo_admin --list             what is in every endpoint's effect chain
apo_admin --install          put our CLSID in the GFX slot, backing up first
apo_admin --uninstall        put back whatever was there
apo_admin --install --legacy the same, with the 2013 CLSID -- for A/B comparison

apo_host --signal sine:1000  drive the DLL with no audiodg.exe in the loop at all
```

`regsvr32` deliberately does not touch any endpoint. Registering makes the class *loadable*;
putting it in an effect chain is a separate act with a far larger blast radius -- it can silence a
machine, it needs the previous value saved, and it should be preceded by a backup. That is
`tools/apo_admin`, which does all three.

## What was dropped from the predecessor

- **Child APO aggregation** (sec. 8.3). The predecessor could chain the OEM's original GFX APO;
  it was never used and its own commit history calls it troublesome.
- **`OriginalGfxApo` at run time.** The value survives only as something the uninstaller restores.
- **Logging to `C:\testbed`**, the `wfstream` tracing, and the dead `AudioIpc.h` legacy
  (`MAIN_SIZE`, `UTILITY_*`, `SRV_WAIT`, `SimpleMutex`) that sec. 4.2 lists as not part of v1.
- **The vendored `Dependencies/baseaudioprocessingobject.h`.** The Windows SDK ships it.

## What was fixed

- **`smartOpen`'s `&&`** (sec. 3.7.3). A sample-rate-only format change now reopens the stream, so
  the header never carries a stale rate. Invisible to a conforming valet, which re-reads the
  header every block anyway; `SyntheticKing` still reproduces the bug, because the client must go
  on tolerating the deployed binary.
- **Real-time safety.** `APOProcess` allocates nothing, locks nothing and logs nothing; the
  `rt/` violation detector is compiled into it, so the sec. 7.4.3 acceptance criterion applies
  inside `audiodg.exe` and not just in the client.
- **Static CRT**, so nothing drags a VC redistributable into a system process. Confirmed: the
  built DLL imports only `AVRT`, `ole32`, `ADVAPI32` and `KERNEL32`.
- **The endpoint keys are left alone.** The predecessor takes ownership of every one and never
  gives it back (sec. 3.7.5). `apo_admin` opens with `KEY_SET_VALUE` and nothing more, which is
  permitted to Administrators outright -- see below.

## Three things that cost a debugging session each

**The audio engine aggregates its APOs.** It calls `IClassFactory::CreateInstance` with a non-null
controlling unknown, and an APO that answers `CLASS_E_NOAGGREGATION` is never instantiated -- the
engine asks for the factory, is refused, and says nothing. The symptom is an APO that is correctly
registered, correctly slotted, demonstrably loaded, and completely inert. The predecessor's
`INonDelegatingUnknown` scaffolding exists for this and not, as it first appeared, for the child
APO. `tests/apo_dll_test.cpp` now drives the factory the way the engine does.

**A populated modern slot makes the GFX slot dead letter.** sec. 3.4 finding 1 in the flesh: a
freshly enumerated endpoint came up with `,5` and `,6` naming Microsoft's own `WM audio LFX APO`
and `WM audio GFX APO`, and with those present *neither* our APO nor the 2013 one ran, despite
`,2` naming them correctly. The endpoint that had worked all along simply had no modern slots.
`apo_admin --install` therefore clears them and saves them, and `--list` shows them, because an
install that silently does nothing is the worst outcome available.

**Ownership of the endpoint key is not needed.** `FxProperties` grants
`BUILTIN\Administrators : SetValue, ReadKey` -- SetValue but not CreateSubKey. `KEY_WRITE` asks
for both and is refused even from an elevated administrator, which is almost certainly how the
predecessor ended up seizing every endpoint key. Asking for `KEY_SET_VALUE` alone works, and
retires sec. 9.4 rather than implementing it.

## Testing

`tests/apo_king_test.cpp` drives `BufferKing` against the production `ipc::BufferValet` under
ctest -- no DLL, no `audiodg.exe`, no elevation. `tests/apo_dll_test.cpp` loads the real built DLL
and exercises its COM surface, aggregation included. Neither needs a sound card.

`tools/apo_host` is the out-of-process driver: it loads an APO DLL, hands it a fabricated
`APOInitSystemEffects`, locks it for a real `IAudioMediaType`, and pumps blocks at the endpoint
clock, with a bank of test signals for metering. It drives the deployed 2013 binary as readily as
this one, which is the point -- a harness proven against a binary nobody here wrote.
