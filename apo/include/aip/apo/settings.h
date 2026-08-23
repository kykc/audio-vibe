// The APO's few settings, read from the registry once per stream.
//
// Read on the control thread, from `Initialize`, and never again: the audio thread reads only
// plain members of a struct that stopped changing before the first block (sec. 7.4.1 forbids
// registry access, and everything else, on that thread).
//
// `audiodg.exe` runs as LOCAL SERVICE, which can read HKLM\SOFTWARE but must not be assumed to
// be able to do anything else. Every value is optional and every failure is a default -- an APO
// that refuses to run because a registry key is missing is an APO that silences the machine.
//
// This is deliberately not the Windows 11 CAPX settings framework (sec. 9.5). That is worth
// adopting when the APO stops being a v1-parity drop-in, and it is contingent on the OS floor
// (sec. 8.1); until then two DWORDs do not justify the dependency.

#pragma once

#include <cstdint>

namespace aip::apo {

/// `HKLM\SOFTWARE\Automatl\AudioIpc`. Absent on a normal machine, which is the intended state:
/// every default below is the shipping behaviour.
extern const wchar_t* const kSettingsKeyPath;

struct Settings {
    /// Whether a `BUFFER_SILENT` block is published to the valet like any other.
    ///
    /// Default **false**, which is the deployed APO's behaviour: it returns early without
    /// publishing anything at all (`AudioIpcApo.cpp:270`). That parity is load-bearing -- the
    /// client's plugin warm-up feeds noise rather than silence precisely because of it
    /// (status.md sec. 7 item 62) -- and it costs nothing while a machine is idle.
    ///
    /// Set the DWORD `ForwardSilentBlocks` to 1 and reverb tails keep ringing and meters keep
    /// moving through silence, at the price of the full rendezvous plus the whole plugin chain
    /// on every idle block, and of a stalled client being able to stall audio when nothing is
    /// even playing. The switch exists so that the choice can be measured rather than argued;
    /// see design_doc.md sec. 9.1 for where it properly belongs.
    bool forwardSilentBlocks = false;

    /// Where control-plane events go. DWORD `Trace`, default **0**, meaning nowhere.
    ///
    ///   1  OutputDebugString (DebugView, or an attached debugger)
    ///   2  a log file at C:\Windows\Temp\aip_apo.log
    ///   3  both
    ///
    /// Control plane only -- `Initialize`, `LockForProcess`, `UnlockForProcess`, and the
    /// registration entry points. Never the audio thread, where I/O of any kind is forbidden
    /// (sec. 7.4.1). Off by default because both sinks are slow and this DLL lives in a process
    /// that must not be slowed down. See `trace.h` for why the file sink exists at all.
    int traceSinks = 0;

    /// Reads the key, substituting the defaults above for anything missing or unreadable.
    /// Control thread only.
    [[nodiscard]] static Settings load() noexcept;
};

} // namespace aip::apo
