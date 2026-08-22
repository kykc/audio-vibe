// What one scan found (design_doc.md sec. 7.2).
//
// Deliberately free of VST3 SDK headers. This is the type the *parent* deals in -- the shell, and
// eventually whatever persists a plugin list -- and neither has any business pulling in the SDK to
// read a name and a vendor. The class id is a string for the same reason; it is only ever handed
// back to `PluginInstance::create` through `VST3::UID::fromString`, in the one process that is
// already an SDK host.
//
// Every field here is filled by a *child* process, because the act of filling it runs third-party
// code that may fault (sec. 7.2). A ScannedModule whose status is not `Ok` is the normal, expected
// outcome for a broken plugin, not an error in this project.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aip::scanner {

/// How the probe of one module ended.
enum class ScanStatus {
    /// The module loaded and every class in it was reported. Individual classes may still carry
    /// their own `error` -- the module was readable, one of its effects was not.
    Ok,
    /// The module loaded but exposed nothing we can host, or `Module::create` refused it. This is
    /// a clean answer from the child, not a failure of the scan.
    LoadFailed,
    /// The child process died while this module was in flight. Whatever it was doing -- loading
    /// the DLL, instantiating, activating -- took the process down with it. The scan continues in
    /// a fresh child; that is what the whole design is for.
    Crashed,
    /// The child made no progress on this module for the scan's per-module deadline and was
    /// terminated. Indistinguishable from a crash to the user and nearly so to us, but recorded
    /// separately because the causes differ: a fault versus a plugin waiting on something.
    TimedOut,
    /// Never reached. Only appears if a scan is cancelled part way through.
    NotProbed,
};

[[nodiscard]] const char* toString(ScanStatus status) noexcept;

/// The inverse, for reading a status back out of anything that stored one as text -- the wire
/// format between the two halves of a scan, and the cached report in the session file. Anything
/// unrecognised reads as `LoadFailed`, which is the safe direction: an entry we cannot understand
/// is one the picker greys out rather than one it offers.
[[nodiscard]] ScanStatus scanStatusFromString(const std::string& text) noexcept;

/// One `kVstAudioEffectClass` inside a module, as the host sees it.
struct ScannedClass {
    /// `VST3::UID::toString()` form. Round-trips through `VST3::UID::fromString`.
    std::string id;
    std::string name;
    std::string vendor;
    std::string version;
    std::string subCategories;

    /// The component and the edit controller are the same object. Changes how the instance is
    /// connected and torn down, and is worth knowing before hosting one (status.md sec. 4).
    bool singleComponent = false;
    /// No edit controller at all, which is legal for an effect with no parameters.
    bool noController = false;
    /// `createView(kEditor)` returned a view. A plugin with no editor is legal and the shell
    /// needs to know before it offers a button that cannot do anything.
    bool hasEditor = false;

    std::int32_t parameterCount = 0;
    std::uint32_t latencySamples = 0;

    std::int32_t audioInputBusses = 0;
    std::int32_t audioOutputBusses = 0;
    std::int32_t mainInputChannels = 0;
    std::int32_t mainOutputChannels = 0;

    /// The class was instantiated *and* prepared at the probe format. False with an empty `error`
    /// means it instantiated but refused the format, which is a legitimate answer from a plugin
    /// that is, say, mono-only.
    bool prepared = false;
    /// See `PluginInstance::fullBusNegotiation` -- a per-plugin quirk, recorded because it is one
    /// of the few things that genuinely varies between hosts of the same plugin.
    bool fullBusNegotiation = false;

    /// Empty unless something went wrong for this class alone.
    std::string error;
};

/// One `.vst3` bundle.
struct ScannedModule {
    std::string path;
    std::string name;
    ScanStatus status = ScanStatus::NotProbed;
    /// The child's diagnostic when `status` is `LoadFailed`; our own when it crashed or hung.
    std::string error;
    std::vector<ScannedClass> classes;

    [[nodiscard]] bool usable() const noexcept { return status == ScanStatus::Ok; }
};

/// Everything one scan produced, in the order the paths were given.
struct ScanReport {
    std::vector<ScannedModule> modules;

    /// Children spawned. One in the happy path; one more for every crash or timeout, which makes
    /// this the cheapest available measure of how hostile the machine's plugin population is.
    int childProcesses = 0;

    [[nodiscard]] std::size_t countWith(ScanStatus status) const noexcept;
};

} // namespace aip::scanner
