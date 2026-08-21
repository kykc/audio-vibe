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
[[nodiscard]] Steinberg::Vst::SpeakerArrangement speakerArrangementFor(
    std::uint32_t channelCount) noexcept;

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
    /// Fails if the plugin will not accept `format.channelCount` on its main busses. That is a
    /// refusal, not a fallback: silently processing a different channel count would corrupt the
    /// planar payload.
    [[nodiscard]] bool prepare(const StreamFormat& format, std::string& error);

    /// Reverses `prepare`. Idempotent.
    void unprepare() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] const StreamFormat& format() const noexcept { return format_; }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

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

    /// Runs one block. `inputs` and `outputs` are arrays of `format().channelCount` channel
    /// pointers, each with at least `frames` floats; they must not alias, because VST3 makes no
    /// promise that a plugin tolerates in-place processing.
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
