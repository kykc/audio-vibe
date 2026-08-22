#include "aip/engine/plugin_instance.h"

#include "public.sdk/source/common/memorystream.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <cassert>
#include <vector>

using Steinberg::kResultOk;
using Steinberg::kResultTrue;
using Steinberg::owned;

namespace Vst = Steinberg::Vst;

namespace aip::engine {
namespace {

// Warmed-up capacity for the parameter-change queues. `ParameterChanges::setMaxParameters`
// preallocates the queue objects, but each queue's point list is a std::vector that only stops
// allocating once it has grown -- so prepare() also pushes and clears this many points per
// queue. Past these bounds a plugin's own automation output would allocate on the audio thread;
// sec. 7.4.5 says that degrades the plugin, and these numbers make it improbable rather than
// impossible.
constexpr Steinberg::int32 kMaxQueuedParameters = 256;
constexpr Steinberg::int32 kMinQueuedParameters = 16;
constexpr Steinberg::int32 kMaxPointsPerParameter = 8;
constexpr Steinberg::int32 kMaxEvents = 128;

// Deactivates every audio bus except the first in each direction. We drive exactly one main
// input and one main output -- protocol v1 has no notion of a side-chain (sec. 4.3) -- so an aux
// bus left active would be asking the plugin to read a buffer we never fill.
void restrictToMainBusses(Vst::IComponent& component) {
    for (Vst::BusDirection dir : {Vst::kInput, Vst::kOutput}) {
        const Steinberg::int32 count = component.getBusCount(Vst::kAudio, dir);
        for (Steinberg::int32 i = 0; i < count; ++i) {
            component.activateBus(Vst::kAudio, dir, i, i == 0 ? 1 : 0);
        }
    }
}

// Pushes `points` values into every one of `count` queues and then clears them, so the
// underlying std::vectors carry capacity into the audio thread. Control thread only.
void warmParameterChanges(Vst::ParameterChanges& changes, Steinberg::int32 count,
                          Steinberg::int32 points) {
    for (Steinberg::int32 p = 0; p < count; ++p) {
        Steinberg::int32 index = 0;
        Vst::IParamValueQueue* queue =
            changes.addParameterData(static_cast<Vst::ParamID>(p), index);
        if (queue == nullptr) {
            break;
        }
        for (Steinberg::int32 i = 0; i < points; ++i) {
            Steinberg::int32 pointIndex = 0;
            queue->addPoint(i, 0.0, pointIndex);
        }
    }
    changes.clearQueue();
}

// Copies the component's state into its controller, which is what makes a separated controller
// show the values the processor actually holds. Best-effort: a plugin that declines is still
// perfectly loadable.
void transferComponentState(Vst::IComponent& component, Vst::IEditController& controller) {
    Steinberg::MemoryStream stream;
    if (component.getState(&stream) != kResultOk) {
        return;
    }
    stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
    controller.setComponentState(&stream);
}

} // namespace

Vst::SpeakerArrangement speakerArrangementFor(std::uint32_t channelCount) noexcept {
    switch (channelCount) {
    case 1:
        return Vst::SpeakerArr::kMono;
    case 2:
        return Vst::SpeakerArr::kStereo;
    case 3:
        return Vst::SpeakerArr::k30Cine;
    case 4:
        return Vst::SpeakerArr::k40Music;
    case 6:
        return Vst::SpeakerArr::k51;
    case 8:
        return Vst::SpeakerArr::k71Cine;
    default:
        break;
    }
    if (channelCount == 0 || channelCount >= 64) {
        return Vst::SpeakerArr::kEmpty;
    }
    return (static_cast<Vst::SpeakerArrangement>(1) << channelCount) - 1;
}

std::unique_ptr<PluginInstance> PluginInstance::create(PluginModule::Ptr module,
                                                       const VST3::UID& classId,
                                                       Steinberg::FUnknown* hostContext,
                                                       std::string& error) {
    error.clear();
    if (!module) {
        error = "no module";
        return nullptr;
    }

    std::unique_ptr<PluginInstance> instance(new PluginInstance());
    instance->module_ = std::move(module);
    instance->classId_ = classId;

    for (const PluginClass& info : instance->module_->audioEffects()) {
        if (info.id == classId) {
            instance->name_ = info.name;
            break;
        }
    }
    if (instance->name_.empty()) {
        error = "class is not an audio effect exposed by " + instance->module_->path();
        return nullptr;
    }

    instance->component_ = instance->module_->factory().createInstance<Vst::IComponent>(classId);
    if (!instance->component_) {
        error = "failed to instantiate " + instance->name_;
        return nullptr;
    }
    if (instance->component_->initialize(hostContext) != kResultOk) {
        // Never terminate() something that failed to initialize: drop the reference instead, or
        // the destructor pairs a terminate with an initialize that never happened.
        instance->component_ = nullptr;
        error = instance->name_ + ": IComponent::initialize failed";
        return nullptr;
    }

    instance->processor_ = Steinberg::U::cast<Vst::IAudioProcessor>(instance->component_);
    if (!instance->processor_) {
        error = instance->name_ + ": component does not implement IAudioProcessor";
        return nullptr;
    }

    // A plugin may be a single object implementing both interfaces, or a pair. Both are legal
    // and they differ in how they are connected and torn down.
    instance->controller_ = Steinberg::U::cast<Vst::IEditController>(instance->component_);
    instance->singleComponent_ = instance->controller_ != nullptr;

    if (!instance->controller_) {
        Steinberg::TUID controllerId;
        if (instance->component_->getControllerClassId(controllerId) == kResultTrue) {
            instance->controller_ =
                instance->module_->factory().createInstance<Vst::IEditController>(
                    VST3::UID::fromTUID(controllerId));
            if (instance->controller_ &&
                instance->controller_->initialize(hostContext) != kResultOk) {
                instance->controller_ = nullptr;
                error = instance->name_ + ": IEditController::initialize failed";
                return nullptr;
            }
        }
    }

    // No controller at all is unusual but legal -- an effect with no parameters. Processing
    // still works; there is simply nothing to edit.
    if (instance->controller_) {
        instance->handler_ = owned(new ComponentHandler());
        instance->controller_->setComponentHandler(instance->handler_);

        if (!instance->singleComponent_) {
            transferComponentState(*instance->component_, *instance->controller_);

            auto componentPoint = Steinberg::U::cast<Vst::IConnectionPoint>(instance->component_);
            auto controllerPoint =
                Steinberg::U::cast<Vst::IConnectionPoint>(instance->controller_);
            if (componentPoint && controllerPoint) {
                instance->componentConnection_ = owned(new Vst::ConnectionProxy(componentPoint));
                instance->controllerConnection_ =
                    owned(new Vst::ConnectionProxy(controllerPoint));
                if (instance->componentConnection_->connect(controllerPoint) != kResultTrue ||
                    instance->controllerConnection_->connect(componentPoint) != kResultTrue) {
                    error = instance->name_ + ": failed to connect component and controller";
                    return nullptr;
                }
            }
        }
    }

    return instance;
}

PluginInstance::~PluginInstance() {
    unprepare();
    disconnect();

    if (controller_ && !singleComponent_) {
        controller_->setComponentHandler(nullptr);
        controller_->terminate();
    }
    controller_ = nullptr;
    handler_ = nullptr;

    processor_ = nullptr;
    if (component_) {
        component_->terminate();
        component_ = nullptr;
    }
    // The module goes last: the DLL must outlive everything created from it.
    module_.reset();
}

void PluginInstance::disconnect() noexcept {
    if (componentConnection_) {
        componentConnection_->disconnect();
        componentConnection_ = nullptr;
    }
    if (controllerConnection_) {
        controllerConnection_->disconnect();
        controllerConnection_ = nullptr;
    }
}

bool PluginInstance::saveState(PluginState& out) const {
    out.component.clear();
    out.controller.clear();

    bool any = false;
    if (component_) {
        Steinberg::MemoryStream stream;
        if (component_->getState(&stream) == kResultOk && stream.getSize() > 0) {
            out.component.assign(stream.getData(), stream.getData() + stream.getSize());
            any = true;
        }
    }
    // Only for a split plugin. A single object implementing both interfaces has exactly one
    // `setState`, and it means the component's -- asking it twice would be asking the same
    // question and restoring it twice would be handing the same blob back under a second name.
    if (controller_ && !singleComponent_) {
        Steinberg::MemoryStream stream;
        if (controller_->getState(&stream) == kResultOk && stream.getSize() > 0) {
            out.controller.assign(stream.getData(), stream.getData() + stream.getSize());
            any = true;
        }
    }
    return any;
}

bool PluginInstance::loadState(const PluginState& state) {
    // See the header: this belongs before prepare(). The assert is the whole enforcement, which
    // is deliberate -- it is live in the configuration we develop in (sec. 6.4) and costs
    // nothing in the one we ship.
    assert(!prepared_ && "loadState must run before prepare()");

    bool ok = true;
    if (component_ && !state.component.empty()) {
        // MemoryStream's read constructor borrows the memory rather than owning it, which is
        // why this const_cast is safe as well as necessary: nothing writes through it.
        Steinberg::MemoryStream stream(const_cast<char*>(state.component.data()),
                                       static_cast<Steinberg::TSize>(state.component.size()));
        if (component_->setState(&stream) != kResultOk) {
            ok = false;
        }
        // The controller gets the same blob, exactly as it does at instantiation. Without this a
        // split plugin restores its sound and opens its editor showing defaults -- the two
        // halves are separate objects and neither tells the other anything.
        if (controller_ && !singleComponent_) {
            stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            controller_->setComponentState(&stream);
        }
    }

    if (controller_ && !singleComponent_ && !state.controller.empty()) {
        Steinberg::MemoryStream stream(const_cast<char*>(state.controller.data()),
                                       static_cast<Steinberg::TSize>(state.controller.size()));
        if (controller_->setState(&stream) != kResultOk) {
            ok = false;
        }
    }
    return ok;
}

bool PluginInstance::prepare(const StreamFormat& format, std::string& error) {
    error.clear();
    unprepare();

    if (!format.valid() || format.channelCount > kMaxChannels) {
        error = name_ + ": invalid stream format";
        return false;
    }
    if (processor_->canProcessSampleSize(Vst::kSample32) != kResultTrue) {
        error = name_ + ": does not support 32-bit float processing";
        return false;
    }

    // Arrangement first: HostProcessData::prepare sizes its channel pointer arrays from
    // getBusInfo, which only reports the negotiated count once setBusArrangements has run.
    const Steinberg::int32 inBusCount = component_->getBusCount(Vst::kAudio, Vst::kInput);
    const Steinberg::int32 outBusCount = component_->getBusCount(Vst::kAudio, Vst::kOutput);
    if (inBusCount < 1 || outBusCount < 1) {
        error = name_ + ": has no main audio input or output bus";
        return false;
    }

    // Describe *every* bus, not just the main pair. A plugin with a side-chain -- and a great
    // many effects have one, default-active -- rejects a partial description outright: it cannot
    // tell "leave the rest alone" from "the rest do not exist". `kEmpty` is VST3's way of saying
    // a bus is not connected, which is exactly true here (protocol v1 carries one stream, sec.
    // 4.3), and is what makes the plugin disable it.
    Vst::SpeakerArrangement arrangement = speakerArrangementFor(format.channelCount);
    std::vector<Vst::SpeakerArrangement> inputs(static_cast<std::size_t>(inBusCount),
                                                Vst::SpeakerArr::kEmpty);
    std::vector<Vst::SpeakerArrangement> outputs(static_cast<std::size_t>(outBusCount),
                                                 Vst::SpeakerArr::kEmpty);
    inputs[0] = arrangement;
    outputs[0] = arrangement;

    fullBusNegotiation_ =
        processor_->setBusArrangements(inputs.data(), inBusCount, outputs.data(), outBusCount) ==
        kResultOk;
    if (!fullBusNegotiation_) {
        // Fall back to describing only the main pair. Some plugins refuse `kEmpty` on a bus they
        // consider mandatory, and would rather be told about one bus than about a disabled one.
        if (processor_->setBusArrangements(&arrangement, 1, &arrangement, 1) != kResultOk) {
            error = name_ + ": rejected a " + std::to_string(format.channelCount) +
                    "-channel bus arrangement";
            return false;
        }
    }

    // Asking is not the same as getting: a plugin may return kResultOk and then report a
    // different arrangement. Only the channel *count* has to agree -- protocol v1 carries no
    // channel-order information -- but it has to agree exactly, because the planar payload is
    // addressed by channel index (sec. 4.3).
    Vst::SpeakerArrangement inArrangement = 0;
    Vst::SpeakerArrangement outArrangement = 0;
    const Steinberg::int32 wanted = static_cast<Steinberg::int32>(format.channelCount);
    if (processor_->getBusArrangement(Vst::kInput, 0, inArrangement) != kResultOk ||
        processor_->getBusArrangement(Vst::kOutput, 0, outArrangement) != kResultOk ||
        Vst::SpeakerArr::getChannelCount(inArrangement) != wanted ||
        Vst::SpeakerArr::getChannelCount(outArrangement) != wanted) {
        error = name_ + ": settled on a bus arrangement that is not " +
                std::to_string(format.channelCount) + " channels";
        return false;
    }

    restrictToMainBusses(*component_);

    Vst::ProcessSetup setup{};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = format.maxFrames;
    setup.sampleRate = static_cast<double>(format.sampleRate);
    if (processor_->setupProcessing(setup) != kResultOk) {
        error = name_ + ": setupProcessing failed";
        return false;
    }

    // bufferSamples = 0: allocate the AudioBusBuffers and their channel *pointer* arrays but no
    // sample storage. The samples live in the chain's scratch banks and in the king's shared
    // mapping; process() only writes the pointers.
    if (!processData_.prepare(*component_, 0, Vst::kSample32)) {
        error = name_ + ": could not prepare the process data";
        return false;
    }
    if (processData_.numInputs < 1 || processData_.numOutputs < 1) {
        error = name_ + ": has no main audio input or output bus";
        return false;
    }

    // Every bus but the main pair is deactivated, and a deactivated bus must still be handed a
    // well-formed AudioBusBuffers -- HostProcessData leaves its channel pointers null. A plugin
    // that reads an inactive bus without checking would then dereference null on the audio
    // thread, taking `audiodg.exe` with it. Pointing them all at one zeroed buffer costs
    // maxFrames floats and removes the whole class of crash. Written once here: process() only
    // ever rewrites bus 0, so these pointers persist.
    //
    // A misbehaving plugin that *writes* to an unused bus scribbles over this shared buffer and
    // so only degrades itself, which is the sec. 7.4.5 bargain.
    unusedBus_.assign(static_cast<std::size_t>(format.maxFrames), 0.0f);
    for (Steinberg::int32 bus = 1; bus < processData_.numInputs; ++bus) {
        for (Steinberg::int32 c = 0; c < processData_.inputs[bus].numChannels; ++c) {
            processData_.inputs[bus].channelBuffers32[c] = unusedBus_.data();
        }
        processData_.inputs[bus].silenceFlags = Vst::HostProcessData::kAllChannelsSilent;
    }
    for (Steinberg::int32 bus = 1; bus < processData_.numOutputs; ++bus) {
        for (Steinberg::int32 c = 0; c < processData_.outputs[bus].numChannels; ++c) {
            processData_.outputs[bus].channelBuffers32[c] = unusedBus_.data();
        }
    }

    const Steinberg::int32 declared =
        controller_ ? controller_->getParameterCount() : Steinberg::int32{0};
    const Steinberg::int32 queueCount =
        std::clamp(declared, kMinQueuedParameters, kMaxQueuedParameters);
    inputParameterChanges_.setMaxParameters(queueCount);
    outputParameterChanges_.setMaxParameters(queueCount);
    warmParameterChanges(inputParameterChanges_, queueCount, kMaxPointsPerParameter);
    warmParameterChanges(outputParameterChanges_, queueCount, kMaxPointsPerParameter);
    queuedParameterLimit_ = queueCount;

    inputEvents_.setMaxSize(kMaxEvents);
    outputEvents_.setMaxSize(kMaxEvents);

    processData_.processMode = Vst::kRealtime;
    processData_.symbolicSampleSize = Vst::kSample32;
    processData_.inputParameterChanges = &inputParameterChanges_;
    processData_.outputParameterChanges = &outputParameterChanges_;
    processData_.inputEvents = &inputEvents_;
    processData_.outputEvents = &outputEvents_;

    if (component_->setActive(true) != kResultOk) {
        error = name_ + ": setActive(true) failed";
        processData_.unprepare();
        return false;
    }
    processor_->setProcessing(true);

    format_ = format;
    prepared_ = true;
    return true;
}

void PluginInstance::unprepare() noexcept {
    if (!prepared_) {
        return;
    }
    processor_->setProcessing(false);
    component_->setActive(false);

    processData_.inputParameterChanges = nullptr;
    processData_.outputParameterChanges = nullptr;
    processData_.inputEvents = nullptr;
    processData_.outputEvents = nullptr;
    processData_.unprepare();

    prepared_ = false;
    queuedParameterLimit_ = 0;
    format_ = StreamFormat{};
}

bool PluginInstance::queueParameter(Vst::ParamID id, Vst::ParamValue value) noexcept {
    if (!inputQueue_.push(ParameterValue{id, value})) {
        droppedParameters_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void PluginInstance::deliverQueuedParameters() noexcept {
    const auto limit = static_cast<std::size_t>(queuedParameterLimit_);
    if (limit == 0) {
        return;
    }
    std::size_t delivered = 0;
    inputQueue_.drain(limit, [this, &delivered](const ParameterValue& queued) {
        Steinberg::int32 index = 0;
        Vst::IParamValueQueue* queue = inputParameterChanges_.addParameterData(queued.id, index);
        if (queue == nullptr) {
            return;
        }
        // Sample offset 0 for every value. The gesture that produced it has no sample-accurate
        // position -- it came from a mouse -- and it is what makes repeated values for one
        // parameter within a block replace each other instead of accumulating points.
        Steinberg::int32 pointIndex = 0;
        queue->addPoint(0, queued.value, pointIndex);
        ++delivered;
    });
    if (delivered != 0) {
        deliveredParameters_.fetch_add(delivered, std::memory_order_relaxed);
    }
}

void PluginInstance::process(float** inputs, float** outputs, std::int32_t frames,
                             Vst::ProcessContext& context) noexcept {
    const Steinberg::int32 channels = static_cast<Steinberg::int32>(format_.channelCount);

    // Pointer stores into arrays prepare() allocated. Nothing here can allocate or block.
    for (Steinberg::int32 c = 0; c < channels; ++c) {
        processData_.inputs[0].channelBuffers32[c] = inputs[c];
        processData_.outputs[0].channelBuffers32[c] = outputs[c];
    }
    processData_.inputs[0].silenceFlags = 0;
    processData_.outputs[0].silenceFlags = 0;

    processData_.numSamples = frames;
    processData_.processContext = &context;

    // The plugin appends to these; leaving the previous block's contents would replay stale
    // automation and stale events.
    outputParameterChanges_.clearQueue();
    outputEvents_.clear();

    // Whatever the control thread has queued since the last block, in the queues the plugin is
    // about to read. This is the only place `inputParameterChanges_` is written.
    deliverQueuedParameters();

    if (processor_->process(processData_) != kResultOk) {
        processFailures_.fetch_add(1, std::memory_order_relaxed);
    }
    processCalls_.fetch_add(1, std::memory_order_relaxed);

    // Consumed by the plugin; the control thread refills them for the next block.
    inputParameterChanges_.clearQueue();
    inputEvents_.clear();
}

} // namespace aip::engine
