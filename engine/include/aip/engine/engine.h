// The control-thread half of the VST3 host (design_doc.md sec. 7.4.3).
//
// Engine is where every forbidden thing happens on purpose: loading DLLs, activating components,
// allocating scratch banks, faulting in pages, destroying retired chains. None of it is reachable
// from the audio thread, which only ever sees the ChainProcessor this object owns.
//
// The division mirrors sec. 7.4.3 exactly:
//
//   Engine (control thread)     load, instantiate, prepare, publish a chain view, destroy what
//                               it replaced, drain plugin callbacks
//   ChainProcessor (audio)      read one pointer, dispatch
//
// **The rack, not the chain, owns the plugins.** Engine holds an ordered rack of instances that
// outlives any number of published chains; a PluginChain is a borrowed view over the subset of
// them that is currently processing. Every mutation below -- insert, remove, move, bypass, and a
// re-prepare for a new stream format -- keeps the instances alive and republishes a view. That is
// what makes a parameter the user set survive the next thing they do; a chain that owned its
// plugins would silently reset the whole rack every time one plugin was added to it.
//
// Engine is not thread-safe against itself. It expects one control thread, which is the same
// assumption ValetSupervisor already makes.

#pragma once

#include "aip/engine/chain_processor.h"
#include "aip/engine/component_handler.h"
#include "aip/engine/plugin_instance.h"
#include "aip/engine/plugin_module.h"
#include "aip/engine/stream_format.h"
#include "aip/rt/realtime_guard.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aip::engine {

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Hand this to ValetSupervisor. It is the only part of the engine the audio thread touches.
    [[nodiscard]] ipc::BlockProcessor& blockProcessor() noexcept { return processor_; }

    [[nodiscard]] ChainProcessor& chainProcessor() noexcept { return processor_; }

    // ------------------------------------------------------------------------- the rack -------
    // Positions are rack positions, bypassed entries included, and stay stable across a rebuild.

    [[nodiscard]] std::size_t pluginCount() const noexcept { return rack_.size(); }

    /// Null when `index` is out of range. The instance stays valid until it is removed or the
    /// Engine is destroyed -- notably, it survives every format change.
    [[nodiscard]] PluginInstance* pluginAt(std::size_t index) const noexcept;

    [[nodiscard]] bool bypassed(std::size_t index) const noexcept;

    // -------------------------------------------------------------------- the chain bypass ----
    //
    // A property of the chain, not of any plugin in it: bypassed, the valet hands the king's own
    // samples straight back, and nothing in the rack runs. It is what a user reaches for to hear
    // what the machine sounds like without this application in the way, so it has to be
    // instantaneous in both directions and cost the rack nothing -- which is why it is a flag the
    // audio thread reads rather than a chain that is unpublished. See ChainProcessor's own note.
    //
    // It survives a rebuild, a format change, an attach and a detach, because none of those is
    // the user changing their mind about it. It is saved with the session and with a preset
    // (config/session.h), for the same reason: it is part of what the chain is, not of what this
    // run happens to be doing.
    //
    // One consequence worth knowing. Nothing drains a plugin's parameter ring while the chain is
    // bypassed, so values pushed at the processor during a bypass are dropped exactly as they are
    // for a bypassed plugin -- see `PluginInstance::droppedParameters`. The controller has them
    // either way, so an editor shows the truth; the processor catches up when the plugin next
    // gets one.

    void setChainBypass(bool bypass) noexcept { processor_.setBypassed(bypass); }

    [[nodiscard]] bool chainBypassed() const noexcept { return processor_.bypassed(); }

    // ------------------------------------------------------------------ mutating the rack -----
    // Each of these takes effect immediately: the rack is edited and a fresh view published, so
    // there is no separate commit step. Sec. 7.4.3 permits these transitions to be audible; what
    // it does not permit is any of the work happening on the audio thread, and none of it does.

    /// Appends the module's first audio-effect class.
    [[nodiscard]] bool appendPlugin(const std::string& path, std::string& error);

    /// Inserts at `index` (clamped to the end). When a stream format is already known the new
    /// plugin is prepared for it here, so a failure is reported before anything is published.
    [[nodiscard]] bool insertPlugin(std::size_t index, const std::string& path,
                                    std::string& error);

    [[nodiscard]] bool insertPlugin(std::size_t index, const std::string& path,
                                    const VST3::UID& classId, std::string& error);

    /// Same, naming the class in its `VST3::UID::toString()` form -- which is what `scanner/`
    /// reports, and the only form a caller that is deliberately not an SDK host can hold. Keeping
    /// the parse here rather than at the call site is the point: `ui/` should not have to include
    /// a VST3 header to say which of a module's classes it means.
    [[nodiscard]] bool insertPluginByClassId(std::size_t index, const std::string& path,
                                             const std::string& classId, std::string& error);

    /// Same again, handing the plugin back the state it was saved with. This exists as its own
    /// entry point rather than as something the caller does afterwards because of *when* the
    /// state has to be applied: between instantiation and `prepare`, and by the time any of the
    /// calls above returns, the instance is already prepared (see PluginInstance::loadState).
    ///
    /// A plugin that rejects the state is still inserted, and the call still returns true --
    /// a rack that restores imperfectly is worth more than a session that refuses to load. When
    /// that happens `error` is non-empty on success, which is the one place in this class where
    /// that is true, and it is how a shell reports "your plugin is here but it starts from
    /// defaults" without calling it a failure.
    [[nodiscard]] bool insertPluginWithState(std::size_t index, const std::string& path,
                                             const std::string& classId, const PluginState& state,
                                             std::string& error);

    /// Removes the plugin at `index` and destroys it, once the audio thread has provably let go.
    [[nodiscard]] bool removePlugin(std::size_t index);

    /// Moves the plugin at `from` so that it ends up at `to`.
    [[nodiscard]] bool movePlugin(std::size_t from, std::size_t to);

    /// A bypassed plugin is left out of the published view entirely -- not asked to bypass
    /// itself. With no latency compensation to preserve (sec. 3.7.1 makes the whole design
    /// zero-latency), skipping it is both simpler and more honest than trusting each plugin's
    /// own bypass parameter.
    [[nodiscard]] bool setBypass(std::size_t index, bool bypass);

    void clearPlugins();

    // ------------------------------------------------------------------------ publication -----

    /// Prepares every rack entry for `format` and publishes a view over the enabled ones. The
    /// instances are re-prepared in place, so their parameters survive; that is the whole reason
    /// the rack outlives the chain.
    ///
    /// On failure nothing is published and audio passes through. It does *not* leave the previous
    /// chain running: re-preparing mutates objects the audio thread could be reading, so the
    /// chain is retracted first.
    [[nodiscard]] bool rebuild(const StreamFormat& format, std::string& error);

    /// Control thread. The endpoint's `dwChannelMask` (`ipc::RenderEndpoint::channelMask`), or
    /// zero for "unknown", which is what a device that reports a plain `WAVEFORMATEX` gives.
    ///
    /// This is the only channel-*order* information in the system: protocol v1 carries none and
    /// its header is frozen (sec. 4.3), so without it a plugin is told which speakers it is
    /// driving by a guess with the right cardinality. It is held across format changes -- the
    /// mask belongs to the device, not to the block geometry -- and takes effect at the next
    /// prepare, so a caller that changes it on a running rack should follow with `rebuild`.
    ///
    /// It can never change how many channels are processed, only which arrangement of them is
    /// asked for first, so a wrong or stale mask costs accuracy and never correctness.
    void setChannelMask(std::uint32_t mask) noexcept { channelMask_ = mask; }

    [[nodiscard]] std::uint32_t channelMask() const noexcept { return channelMask_; }

    /// Builds for a geometry we have *guessed* rather than observed, so that a rack is ready
    /// before any audio has arrived. `sampleRate` and `channelCount` come from the endpoint's
    /// configured device format (`ipc::RenderEndpoint`).
    ///
    /// Protocol v1 announces the format nowhere (sec. 4.5), so without this a client that attaches
    /// while nothing is playing sits with every plugin unprepared until the user happens to play
    /// something -- and only then finds out that one of them refuses the format, and only then
    /// runs the warm-up. Guessing moves both to the moment they pressed Attach.
    ///
    /// A wrong guess is already a handled case rather than a hazard: ChainProcessor compares each
    /// block against the format the chain was built for and passes it through untouched on a
    /// mismatch, and `serviceFormatChange` then rebuilds from what was actually observed. The
    /// cost of guessing wrong is one passed-through block and one rebuild -- which is exactly
    /// what a format change costs anyway.
    ///
    /// Deliberately does not touch `servicedFormatKey_`: the first real block must still be
    /// examined, because it is the only thing that can confirm or contradict the guess.
    ///
    /// A no-op returning true when the guess matches what is already built. Returns false with
    /// `error` set when the geometry is unusable or a plugin refused it, exactly as `rebuild`
    /// does -- and a refusal here is the point, because it is being reported hours earlier than
    /// it otherwise would be.
    [[nodiscard]] bool prepareSpeculatively(std::uint32_t sampleRate, std::uint32_t channelCount,
                                            std::string& error);

    /// True when the built format came from `prepareSpeculatively` and no block has confirmed it
    /// yet. Worth showing: "prepared" on a guess is a weaker claim than "prepared" on a block.
    [[nodiscard]] bool builtFormatIsSpeculative() const noexcept { return speculative_; }

    /// Unpublishes the chain. The rack is untouched, so a later `rebuild` brings it back with
    /// every parameter still set.
    void teardown();

    /// Builds or rebuilds when the audio thread has reported a block geometry the published
    /// chain cannot run -- including the very first one, where there is no chain at all. Call it
    /// from the control thread's polling loop; it is cheap when nothing has changed.
    ///
    /// Returns true when a rebuild happened and succeeded. Returns false either because nothing
    /// needed doing -- `error` empty -- or because the rebuild failed, in which case `error` says
    /// why and audio keeps passing through rather than stopping.
    bool serviceFormatChange(std::string& error);

    [[nodiscard]] const StreamFormat& builtFormat() const noexcept { return builtFormat_; }

    // ---------------------------------------------------------------------------- warm-up -----

    /// Blocks each plugin is run for the moment it is prepared. Four rather than one because
    /// first-call laziness is not always on the *first* call -- a plugin that fills an internal
    /// buffer before it allocates gets there on the second or third.
    static constexpr std::size_t kWarmUpBlocks = 4;

    /// What the last warm-up did, summed over the plugins it covered. See
    /// `PluginInstance::warmUp` for what is being warmed and why, and
    /// `PluginInstance::WarmUpResult::violations` for the one thing this cannot tell you: what
    /// the plugin itself allocated. The detector does not reach inside a plugin DLL, so a
    /// nonzero `violations` here means *our* processing path misbehaved, which is a defect.
    struct WarmUpReport {
        std::size_t plugins = 0;
        std::size_t blocks = 0;
        std::size_t blocksFailed = 0;
        rt::ViolationCounts violations;

        [[nodiscard]] bool ran() const noexcept { return plugins != 0; }
    };

    [[nodiscard]] const WarmUpReport& lastWarmUp() const noexcept { return lastWarmUp_; }

    // ------------------------------------------------------------------ plugin callbacks ------

    /// Drains what the plugins queued through IComponentHandler and applies each edit to the half
    /// of the plugin that did not originate it: an edit the processor made is pushed into the
    /// controller, and an edit the editor made is queued for the processor. That second direction
    /// is what makes a knob in a plugin's own editor change the audio at all when the component
    /// and the controller are separate objects. `maxPerPlugin` bounds the work, as sec. 7.4.2
    /// asks even of the control thread. Returns the number of edits consumed.
    ///
    /// This can rebuild the chain, and that is not a surprise smuggled into a drain: a
    /// `restartComponent` request is one of the things the plugins queue here, and honouring one
    /// means re-preparing the rack. See `RestartReport` for what is acted on and what is not, and
    /// `lastRestart()` for what this call did about it.
    std::size_t serviceParameterEdits(std::size_t maxPerPlugin = 64);

    /// What the plugins asked for through `restartComponent` during the last
    /// `serviceParameterEdits`, folded together across the rack. Reset at the start of every such
    /// call, so it describes that one servicing tick and not the run.
    ///
    /// The flags are grouped by what this host can actually do about them:
    ///
    ///   reconfiguration   `kReloadComponent`, `kIoChanged`, `kLatencyChanged` -- the plugin is
    ///                     asking to be deactivated and reactivated. That is what `rebuild` does
    ///                     to every instance in the rack, so they go through the same path a
    ///                     format change takes, at the format already built.
    ///   informational     `kParamValuesChanged` -- the plugin moved its own parameters and the
    ///                     host's caches are stale. There is no cache here; the values live in
    ///                     the controller, which is where anything showing them reads from. So
    ///                     this is passed on to be *told*, and nothing in the engine changes.
    ///   the rest          named in `unhandled` and otherwise ignored. Titles, MIDI-CC mappings,
    ///                     note expression and routing all describe things this host does not
    ///                     model at all, and pretending to act on them would be a lie in a log.
    ///
    /// `kReloadComponent` is honoured as a re-prepare rather than as the full unload-and-reload
    /// the SDK describes. The instance, its parameters and its state survive; what it gets is a
    /// deactivate, a fresh bus negotiation, a `setupProcessing` and a reactivate. No plugin met so
    /// far has asked for more, and the difference is worth knowing about before one does.
    struct RestartReport {
        /// `restartComponent` calls drained this tick, across every plugin in the rack.
        std::size_t requests = 0;
        /// The union of every flag in them, raw, as the plugins gave it.
        std::int32_t flags = 0;
        /// A plugin moved its own parameter values and said so. Nothing to do here; somebody
        /// displaying them needs to know.
        bool parameterValues = false;
        /// A reconfiguration was asked for and the rack was rebuilt for it. False when none was
        /// asked for, when there was no format to rebuild at -- nothing is prepared, so there is
        /// nothing to deactivate -- or when the rebuild failed, which `error` then says.
        bool reconfigured = false;
        /// Reconfiguration requests dropped because they arrived out of the rebuild that was
        /// already running for one. See the note in `serviceParameterEdits`: a plugin announcing
        /// its new latency from inside its own reactivation is ordinary, and acting on it would
        /// rebuild forever.
        std::size_t suppressed = 0;
        /// Why a demanded reconfiguration did not happen. Empty when none was asked for, or when
        /// it worked.
        std::string error;
        /// Flags seen that this host does not act on, spelled out for a log. Empty when there
        /// were none. Built here rather than by the caller because naming a VST3 flag needs a
        /// VST3 header, and `ui/` deliberately does not have one.
        std::string unhandled;

        [[nodiscard]] bool any() const noexcept { return requests != 0; }
    };

    [[nodiscard]] const RestartReport& lastRestart() const noexcept { return lastRestart_; }

    /// Edits dropped across the whole rack because an audio-thread ring filled up. Nonzero means
    /// `serviceParameterEdits` is not being called often enough.
    [[nodiscard]] std::uint64_t droppedParameterEdits() const;

    /// Values carried across to the processors through `inputParameterChanges`, rack-wide.
    [[nodiscard]] std::uint64_t deliveredParameters() const;

    /// Values that never got there because a ring was full -- in practice because no chain is
    /// published, so nothing is draining. Rack-wide.
    [[nodiscard]] std::uint64_t droppedParameters() const;

    /// Instances that could not be destroyed when they were removed, because the audio thread had
    /// not left the chain naming them within the grace period. They are freed at teardown. A
    /// nonzero count means the valet thread was wedged, not that anything leaked silently.
    [[nodiscard]] std::size_t strandedPlugins() const noexcept { return stranded_.size(); }

private:
    struct RackEntry {
        std::unique_ptr<PluginInstance> instance;
        bool bypassed = false;
    };

    [[nodiscard]] PluginModule::Ptr moduleFor(const std::string& path, std::string& error);

    /// The one insertion path; everything public above narrows down to it. `state` is optional
    /// and applied before `prepare`.
    [[nodiscard]] bool insertPluginImpl(std::size_t index, const std::string& path,
                                        const VST3::UID& classId, const PluginState* state,
                                        std::string& error);

    /// Publishes a view over every prepared, non-bypassed rack entry. Publishes nothing when no
    /// format is known yet. Returns false only if the audio thread failed to release the chain
    /// it replaced, which is reported rather than waited on forever.
    bool publishRack();

    /// Retracts the chain so that rack instances can be mutated safely.
    bool retract();

    /// What one drain pass over the rack found. Separated from `RestartReport` because a single
    /// `serviceParameterEdits` makes two passes when it rebuilds, and the second one is counted
    /// differently from the first.
    struct DrainTally {
        std::size_t edits = 0;
        std::size_t restartRequests = 0;
        /// Restart requests among those that asked for a deactivate/reactivate.
        std::size_t reconfigurationRequests = 0;
        /// The union of every restart flag seen.
        std::int32_t flags = 0;
    };

    /// One pass over every rack entry: applies each parameter edit to the half of the plugin that
    /// did not originate it, and folds the restart requests into the tally rather than acting on
    /// them. Acting is the caller's job, because it can mean rebuilding the whole rack and that
    /// cannot happen while a handler is being drained.
    DrainTally drainRack(std::size_t maxPerPlugin);

    // Declaration order is load-bearing -- members are destroyed in reverse, and that reverse
    // order is the only safe one. See the note in ~Engine before changing any of it.
    Steinberg::IPtr<Steinberg::Vst::HostApplication> host_;
    /// Keyed by path, so adding a second instance of a plugin does not reload the same DLL.
    std::map<std::string, PluginModule::Ptr> modules_;
    std::vector<RackEntry> rack_;
    std::vector<std::unique_ptr<PluginInstance>> stranded_;

    ChainProcessor processor_;
    /// Runs `instance` through kWarmUpBlocks and folds the result into `lastWarmUp_`. The
    /// caller owes `PluginInstance::warmUp` its precondition: no published chain may name the
    /// instance.
    void warmUpInstance(PluginInstance& instance);

    StreamFormat builtFormat_;
    /// See builtFormatIsSpeculative(). Cleared the moment a real block confirms the geometry.
    bool speculative_ = false;
    WarmUpReport lastWarmUp_;
    RestartReport lastRestart_;
    /// See setChannelMask(). Survives every rebuild.
    std::uint32_t channelMask_ = 0;
    /// The packed geometry key the last `serviceFormatChange` acted on. See that function.
    std::uint64_t servicedFormatKey_ = 0;
};

} // namespace aip::engine
