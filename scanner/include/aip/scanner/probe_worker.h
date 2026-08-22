// The half of the scanner that runs *inside* the child process (design_doc.md sec. 7.2).
//
// Everything declared here loads third-party DLLs, activates COM objects and calls into plugin
// code. Do not call it from the shell, the tests' own process, or anything holding a valet thread:
// a plugin that faults during `create` or `setActive` takes the whole process with it, and that is
// precisely the event the scanner is built to absorb rather than avoid.
//
// The header stays free of VST3 headers so the parent can share the options type without becoming
// an SDK host.

#pragma once

#include "aip/scanner/scan_result.h"

#include <cstdint>
#include <string>

namespace aip::scanner {

/// What the child does to each candidate.
struct ProbeOptions {
    /// The geometry a class is asked to prepare for. Nothing has told us the endpoint's real
    /// format -- protocol v1 announces it nowhere (sec. 4.5) -- so a nominal one is used, and a
    /// class that refuses it is recorded as refusing rather than treated as broken.
    std::uint32_t sampleRate = 48000;
    std::uint32_t channelCount = 2;

    /// The endpoint's `dwChannelMask`, or zero for "unknown" -- which is the default here, and
    /// honest: a scan is not attached to any endpoint. It only selects which arrangement of
    /// `channelCount` channels a class is offered first, so leaving it zero costs the probe the
    /// tier-1 attempt and nothing else.
    std::uint32_t channelMask = 0;

    /// Instantiate and activate, not merely load. This is where most of the crash surface is --
    /// `setupProcessing`, `setActive`, and whatever a plugin does on first activation -- and also
    /// where `fullBusNegotiation` becomes knowable, so it is on by default. Turning it off makes
    /// a scan much faster and much less informative.
    bool prepare = true;

    /// Ask each class for an editor view. Cheap for most plugins and the only way to know whether
    /// the shell can offer an Edit button, but it is more third-party code running, so it is
    /// separable from the rest.
    bool queryEditor = true;
};

/// Probes one `.vst3` bundle and returns what was learned. Never throws and never reports a crash
/// -- a crash here does not return.
///
/// The returned status is `Ok` or `LoadFailed`; `Crashed` and `TimedOut` are verdicts only the
/// parent can reach, because they are precisely the cases where this function does not return.
[[nodiscard]] ScannedModule probeModule(const std::string& path, const ProbeOptions& options);

/// Turns a fault into an exit code instead of a dialog box.
///
/// Not cosmetic. Windows Error Reporting, the CRT's abort dialog and the "abnormal program
/// termination" box all *block on user input*, which converts a crash the parent handles in
/// milliseconds into a hang it can only resolve by timing out -- and, if a scan runs unattended,
/// into a process sitting there for as long as the machine is up. The child must die quietly.
void suppressCrashDialogs() noexcept;

} // namespace aip::scanner
