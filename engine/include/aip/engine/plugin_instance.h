// One instantiated VST3 plugin, prepared for a fixed stream format (design_doc.md sec. 7.4.5).
//
// The split by thread is the whole point of the class:
//
//   control thread   create / prepare / unprepare / destructor -- allocates, loads, activates
//   audio thread     process() -- a pointer fix-up and one call into the plugin, nothing else
//
// Everything `process()` touches is allocated and page-touched by `prepare()`: the
// AudioBusBuffers and their channel pointer arrays (via HostProcessData), the input and output
// IParameterChanges, and the IEventLists. Sec. 7.4.5 requires exactly that -- call `process`
// directly, wrapped in nothing that allocates, locks, or copies through a dynamic buffer.
//
// What the plugin itself does inside `process` is outside our control and outside our
// obligation. A misbehaving plugin degrades that plugin; it must not make *our* code violate
// sec. 7.4.1.

#pragma once

#include "aip/engine/component_handler.h"
#include "aip/engine/plugin_module.h"
#include "aip/engine/stream_format.h"
#include "aip/rt/spsc_queue.h"

#include "public.sdk/source/vst/hosting/connectionproxy.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aip::engine {

/// Maps a channel count onto a VST3 speaker arrangement. Only the *count* has to agree for the
/// protocol's planar payload to line up (sec. 4.3) -- protocol v1 carries no channel-order
/// information at all -- so counts without a standard layout fall back to the low `n` speaker
/// bits, which is arbitrary but has the right cardinality.
///
/// This is tier 2 of the negotiation: a guess with the right cardinality, used when the device
/// declines to say what its channels mean.
[[nodiscard]] Steinberg::Vst::SpeakerArrangement speakerArrangementFor(
    std::uint32_t channelCount) noexcept;

/// Maps a Windows `dwChannelMask` onto the VST3 speaker arrangement that means the same thing,
/// and is tier 1 of the negotiation: the only path that gets channel *roles* right rather than
/// merely getting the count right.
///
/// The mapping is the identity on the bits, which is not a coincidence -- VST3 numbered its
/// first eighteen speakers to match the `SPEAKER_*` order exactly, from `kSpeakerL` at bit 0
/// through `kSpeakerTrr` at bit 17. So this function is mostly validation:
///
///   * a mask whose population count disagrees with `channelCount` is discarded. The device
///     format describes what the endpoint is *configured* for, and the count in front of the
///     audio thread is read from the block header every block; when they disagree the header
///     wins and the mask is not about this stream.
///   * a mask with bits above 17 set is discarded. Windows defines no speaker there, so
///     whatever it is, it is not something we can translate.
///   * one channel returns `kMono`. Windows spells mono `SPEAKER_FRONT_CENTER`, which maps
///     bit-wise onto `kSpeakerC` -- a *centre* channel of a surround layout, not a mono stream.
///     Plugins overwhelmingly expect `kMono`, and the identity would quietly deny mono plugins
///     the arrangement they are looking for.
///
/// Returns `kEmpty` when the mask is unusable, which is the caller's signal to fall to tier 2.
[[nodiscard]] Steinberg::Vst::SpeakerArrangement speakerArrangementForMask(
    std::uint32_t channelMask, std::uint32_t channelCount) noexcept;

/// A plugin's own persistent state, exactly as the plugin wrote it and opaque to us.
///
/// Two blobs, because a split component/controller plugin keeps two. The component's state is
/// the one that matters -- it is what makes the audio come back the way the user left it. The
/// controller's is the editor's own business: which page was open, how the analyser was scaled,
/// anything the plugin does not consider part of its sound. A plugin whose component *is* its
/// controller has only the first; `controller` stays empty for one of those and nothing tries to
/// restore it, because a single object cannot have two meanings for `setState`.
struct PluginState {
    std::vector<char> component;
    std::vector<char> controller;

    [[nodiscard]] bool empty() const noexcept {
        return component.empty() && controller.empty();
    }
};

class PluginInstance {
public:
    /// Control thread. Instantiates the component, initialises it against `hostContext`, pairs
    /// it with its edit controller, transfers component state to the controller and connects
    /// the two through the SDK's ConnectionProxy. Returns null and fills `error` on failure.
    ///
    /// `module` is held for the lifetime of the instance: unloading the DLL while a component
    /// from it is alive is a use-after-free.
    [[nodiscard]] static std::unique_ptr<PluginInstance> create(PluginModule::Ptr module,
                                                                const VST3::UID& classId,
                                                                Steinberg::FUnknown* hostContext,
                                                                std::string& error);

    ~PluginInstance();

    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;

    // ---------------------------------------------------------------- control thread ---------

    /// Negotiates the bus arrangement, runs `setupProcessing`, allocates every structure
    /// `process()` will use, then `setActive(true)` and `setProcessing(true)`. This is the
    /// sanctioned place for a plugin to allocate (sec. 7.4.3, step 1).
    ///
    /// `channelMask` is the endpoint's `dwChannelMask` (`ipc::RenderEndpoint::channelMask`), or
    /// zero when the device did not say. It only ever selects *which* arrangement of
    /// `format.channelCount` channels is asked for first; it can never change how many channels
    /// are processed, so a stale or absent mask costs accuracy of channel roles and nothing else.
    ///
    /// The arrangement is negotiated in three tiers, each tried only when the one before it
    /// fails:
    ///
    ///   1. the arrangement the device's mask names -- the only tier that gets channel roles
    ///      right, and the reason `channelMask` is threaded down this far
    ///   2. `speakerArrangementFor(format.channelCount)` -- right cardinality, guessed roles
    ///   3. whatever fixed arrangement the plugin insists on, provided it is *at least*
    ///      `format.channelCount` wide, with the surplus channels fed silence and their outputs
    ///      discarded. See `inputChannelCount`.
    ///
    /// Fails if the plugin will not accept at least `format.channelCount` channels on its main
    /// busses. That is a refusal, not a fallback: a plugin narrower than the stream would have to
    /// drop channels, and silently dropping channels corrupts the planar payload.
    [[nodiscard]] bool prepare(const StreamFormat& format, std::uint32_t channelMask,
                               std::string& error);

    /// Reverses `prepare`. Idempotent.
    void unprepare() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] const StreamFormat& format() const noexcept { return format_; }

    /// Channels the plugin settled on for its main input and output bus, which is what `process`
    /// requires its `inputs` and `outputs` arrays to be wide -- *not* `format().channelCount`.
    ///
    /// These are equal to `format().channelCount` for every plugin that accepted the stream's
    /// width, which is nearly all of them. They exceed it for a plugin with a fixed wider bus
    /// (tier 3 above): channels `[format().channelCount, inputChannelCount())` are padding the
    /// caller must supply as silence, and the matching output channels carry whatever the plugin
    /// made of that silence and are to be discarded.
    ///
    /// The numbers come from the prepared AudioBusBuffers rather than from the arrangement we
    /// negotiated, because it is the buffers that size the channel pointer arrays `process`
    /// writes into -- and a plugin whose `getBusInfo` disagrees with its own arrangement would
    /// otherwise have us write past them.
    ///
    /// Zero until `prepare` has succeeded. The two may differ from each other.
    [[nodiscard]] std::uint32_t inputChannelCount() const noexcept { return inputChannels_; }

    [[nodiscard]] std::uint32_t outputChannelCount() const noexcept { return outputChannels_; }

    /// True when the plugin was given a wider bus than the stream and is being fed silence in
    /// the surplus channels. Worth surfacing: it is the condition under which a plugin that
    /// links its detector across channels sees a quieter signal than the stream actually
    /// carries, and the only visible symptom is that it acts too gently.
    [[nodiscard]] bool padded() const noexcept {
        return prepared_ && (inputChannels_ > format_.channelCount ||
                             outputChannels_ > format_.channelCount);
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// The module this instance came from, as the SDK resolved it -- which is absolute, and
    /// therefore what a session file should record rather than whatever relative path a caller
    /// happened to pass in.
    [[nodiscard]] const std::string& path() const noexcept { return module_->path(); }

    [[nodiscard]] const VST3::UID& classId() const noexcept { return classId_; }

    /// The class id in the form `scanner/` reports and `insertPluginByClassId` accepts, which is
    /// also the only form worth writing to a text file.
    [[nodiscard]] std::string classIdString() const { return classId_.toString(); }

    // ------------------------------------------------------------- the plugin's own state -----

    /// Control thread. Asks the plugin for its state. False means it gave us nothing: legal, and
    /// not a failure worth putting in front of a user -- an effect with no parameters has no
    /// state to save, and a plugin is entitled to decline. `out` is cleared either way.
    [[nodiscard]] bool saveState(PluginState& out) const;

    /// Control thread, on an instance that has **not** been prepared yet. False means the plugin
    /// rejected a blob it was given, which is worth reporting: it usually means the state came
    /// from a different version of the plugin.
    ///
    /// The ordering obligation is real. VST3 permits `setState` on an active component and hosts
    /// do it -- that is what preset recall during playback is -- but a plugin is entitled to
    /// expect it before `setupProcessing`, and restoring state into an instance that has already
    /// negotiated its busses is a needlessly hostile way to find out which plugins disagree.
    /// `Engine::insertPluginWithState` is the path that gets this right; asserted here so a
    /// second caller cannot get it wrong quietly.
    [[nodiscard]] bool loadState(const PluginState& state);

    /// Null when the plugin exposes no edit controller, which is legal for an effect with no
    /// parameters. There is nothing to drain in that case.
    [[nodiscard]] ComponentHandler* handler() noexcept { return handler_; }

    [[nodiscard]] Steinberg::Vst::IEditController* controller() const noexcept {
        return controller_;
    }

    [[nodiscard]] Steinberg::Vst::IComponent* component() const noexcept { return component_; }

    /// True when the plugin accepted a description of *all* its busses, with everything but the
    /// main pair set to `kEmpty`. False means it only accepted a description of the main pair and
    /// its auxiliary busses are still nominally connected -- worth knowing per plugin, because it
    /// is the difference between a side-chain the plugin has disabled and one it merely ignores.
    /// Meaningful only once `prepare` has succeeded.
    [[nodiscard]] bool fullBusNegotiation() const noexcept { return fullBusNegotiation_; }

    // ------------------------------------------------- parameters into the processor ---------
    //
    // A plugin's editor talks to its *controller*. For a split component/controller plugin the
    // two halves are separate objects that are forbidden to talk to each other directly, so a
    // knob the user turns changes nothing audible until the host carries the value across --
    // through `IProcessData::inputParameterChanges`, which only the audio thread may write,
    // because the plugin reads it during `process`.
    //
    // Hence a ring: the control thread pushes values in, `process` drains a bounded number of
    // them into the preallocated queues at the top of the next block. Values are stamped at
    // sample offset 0 and therefore coalesce -- the SDK's `addPoint` replaces a point at an
    // offset it already holds -- so a fast gesture costs one queue slot per parameter per block
    // however hard it is dragged, and nothing grows.

    /// Control thread. Queues a normalized value for delivery to the processor on the next
    /// block. False means the ring was full and the value was dropped: for a UI gesture that is
    /// benign, because the next mouse move supersedes it, and the alternative is blocking a
    /// thread that must never block.
    [[nodiscard]] bool queueParameter(Steinberg::Vst::ParamID id,
                                      Steinberg::Vst::ParamValue value) noexcept;

    /// Values handed to the plugin through `inputParameterChanges`.
    [[nodiscard]] std::uint64_t deliveredParameters() const noexcept {
        return deliveredParameters_.load(std::memory_order_relaxed);
    }

    /// Values dropped because the ring was full. Nonzero means the audio thread is not draining
    /// -- no chain published, or the plugin bypassed -- rather than that anything is too slow.
    [[nodiscard]] std::uint64_t droppedParameters() const noexcept {
        return droppedParameters_.load(std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------ audio thread ---------

    /// Runs one block. `inputs` is an array of `inputChannelCount()` channel pointers and
    /// `outputs` of `outputChannelCount()`, each with at least `frames` floats; they must not
    /// alias, because VST3 makes no promise that a plugin tolerates in-place processing.
    ///
    /// Those widths are the plugin's, not the stream's. Sizing either array to
    /// `format().channelCount` when the plugin settled on a wider bus hands the plugin the null
    /// pointers HostProcessData left in the tail of its channel array, on the audio thread,
    /// inside `audiodg.exe`.
    ///
    /// Allocation-free and lock-free on our side of the call. Not `noexcept` by accident: it is
    /// noexcept because unwinding through the audio thread is forbidden (sec. 7.4.1), and a
    /// plugin that throws across the ABI boundary is already beyond saving.
    void process(float** inputs, float** outputs, std::int32_t frames,
                 Steinberg::Vst::ProcessContext& context) noexcept;

    [[nodiscard]] std::uint64_t processCalls() const noexcept {
        return processCalls_.load(std::memory_order_relaxed);
    }

    /// Blocks the plugin returned something other than `kResultOk` for. Non-fatal by design --
    /// we keep the audio flowing and let the control plane notice.
    [[nodiscard]] std::uint64_t processFailures() const noexcept {
        return processFailures_.load(std::memory_order_relaxed);
    }

    /// One queued normalized value, flattened so it can live in a fixed-capacity ring.
    struct ParameterValue {
        Steinberg::Vst::ParamID id = 0;
        Steinberg::Vst::ParamValue value = 0.0;
    };

    /// Enough for a full block's worth of a user dragging several controls at once, and small
    /// enough that the ring stays a cache-friendly few kilobytes.
    static constexpr std::size_t kInputQueueSlots = 1024;

private:
    PluginInstance() = default;

    void disconnect() noexcept;

    /// Audio thread. Moves at most `queuedParameterLimit_` values out of the ring and into
    /// `inputParameterChanges_`. The bound is what keeps `addParameterData` inside the queue
    /// objects `prepare` warmed, so nothing here can allocate (sec. 7.4.1).
    void deliverQueuedParameters() noexcept;

    PluginModule::Ptr module_;
    std::string name_;
    VST3::UID classId_;

    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_;
    Steinberg::IPtr<Steinberg::Vst::ConnectionProxy> componentConnection_;
    Steinberg::IPtr<Steinberg::Vst::ConnectionProxy> controllerConnection_;
    Steinberg::IPtr<ComponentHandler> handler_;
    /// True when the component *is* its own controller, which changes how it must be terminated.
    bool singleComponent_ = false;

    StreamFormat format_;
    bool prepared_ = false;
    bool fullBusNegotiation_ = false;
    /// The negotiated main-bus widths. See inputChannelCount().
    std::uint32_t inputChannels_ = 0;
    std::uint32_t outputChannels_ = 0;
    /// Silence bits for the padding channels of the main input bus, computed once by prepare()
    /// so process() only has to store them. Zero whenever the plugin took the stream's width.
    std::uint64_t inputSilenceFlags_ = 0;

    /// Zeroed backing for every channel of every bus we do not drive. See prepare().
    std::vector<float> unusedBus_;

    // Everything below is allocated by prepare() and only read/written by process().
    Steinberg::Vst::HostProcessData processData_;
    Steinberg::Vst::ParameterChanges inputParameterChanges_;
    Steinberg::Vst::ParameterChanges outputParameterChanges_;
    Steinberg::Vst::EventList inputEvents_;
    Steinberg::Vst::EventList outputEvents_;

    /// Producer is the control thread, consumer is the audio thread inside `process`.
    rt::SpscQueue<ParameterValue, kInputQueueSlots> inputQueue_;
    /// How many distinct parameters `inputParameterChanges_` was warmed for, and therefore the
    /// per-block drain bound. Set by `prepare`.
    Steinberg::int32 queuedParameterLimit_ = 0;

    std::atomic<std::uint64_t> processCalls_{0};
    std::atomic<std::uint64_t> processFailures_{0};
    std::atomic<std::uint64_t> deliveredParameters_{0};
    std::atomic<std::uint64_t> droppedParameters_{0};
};

} // namespace aip::engine
