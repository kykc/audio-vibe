// A VST3 plugin that exists only to be hosted by our own tests.
//
// Depending on a third-party plugin, or on one of the SDK's samples, would make the engine tests
// depend on something we do not control and cannot make misbehave on demand. This fixture is the
// opposite: every number it produces is predictable from the input, and the behaviour the host
// has to survive is switchable through a parameter.
//
// It is a *single component* -- one object implementing both IComponent and IEditController --
// because that is the configuration in which a plugin can reach IComponentHandler from its own
// processing thread. Sec. 7.4.5 of design_doc.md makes tolerating that our obligation, so the
// fixture does it deliberately: set `Edits` above zero and every `process` call issues that many
// `performEdit` callbacks from the audio thread.
//
// Behaviour, in full:
//
//   Gain    (id 0)  output = input * (normalized * 2), so the 0.5 default is unity
//   Edits   (id 1)  performEdit calls per process() = round(normalized * 8), from the audio thread
//   Meter   (id 2)  the parameter those callbacks report; its value is the block's first sample
//   Offset  (id 3)  a constant added *after* the gain; 0 by default
//   Latency (id 4)  reported latency = round(normalized * 512) samples, announced with
//                   restartComponent(kLatencyChanged) and *not* readable until reactivation
//   Restart (id 5)  restartComponent(round(normalized * 2047)) -- the flag word, raw
//   Echo    (id 6)  above 0.5, every setActive(true) announces kLatencyChanged again
//
// The last three are what a host's restart handling has to be pointed at, and none of them is
// saved in the state: they are triggers, not settings.
//
// Latency models the SDK's contract rather than the convenient version of it. `getLatencySamples`
// keeps returning the *old* figure until `setActive(true)` happens, which is exactly what the
// interface says ("should return the new latency after setActive (true) was called"). A host that
// hears kLatencyChanged and merely re-reads the number therefore reads a stale one, and a host
// that deactivates and reactivates first does not -- so the fixture can tell those two hosts
// apart, which is the whole point of having it.
//
// Echo is the plugin that makes a naive host loop forever. Announcing a latency change from
// inside one's own reactivation is not misbehaviour -- a JUCE wrapper does it from
// `prepareToPlay`, which is where it learns its own latency -- but a host that reacts to every
// such announcement by reactivating produces another one, and never stops. Off by default,
// because it is the pathological case rather than the common one.
//
// Restart takes the flag word directly, so a test can raise any combination it likes -- including
// the ones this host deliberately does nothing about, which are as much a part of the behaviour
// as the ones it acts on. The span covers every flag the SDK defines, `kReloadComponent` through
// `kKeyswitchChanged`, so the value to send for a flag word F is F / kRestartFlagSpan.
//
// Its main busses are declared mono and accept anything up to eight channels, which is the shape
// of plugin that catches a host skipping the negotiation altogether: one that asks for nothing
// and runs on whatever the plugin came up with gets a mono bus and a stereo stream.
//
// It also has real state: `getState` writes a magic number and the three writable parameters,
// `setState` reads them back, and a blob whose magic does not match is *refused*. Both halves
// matter to the host. Without persistence a session test could only assert that a plugin comes
// back, not that it comes back as it was; without the refusal there would be nothing to point
// Engine::insertPluginWithState's "loaded, but would not take its state" path at, and that path
// is the one a user meets after updating a plugin.
//
// Offset exists so that two instances do not commute: gain-then-offset and offset-then-gain give
// different numbers, which is what lets a test tell a correctly ordered chain from a reversed
// one. Two gains alone cannot -- multiplication does not care about order, so a rack that ran
// them backwards would produce byte-identical output and the test would pass anyway.
//
// The module carries a *second* class, `AIP Wide Plugin`, which exists for one reason: it will
// not accept any bus arrangement other than its own eight channels. That is the shape of plugin
// -- common enough, Voxengo's are the usual specimens -- that a host either pads up to or refuses
// outright, and the padding path in PluginInstance::prepare has nothing to test against without
// one. It sums its whole input bus into every output channel, so a host that lets the padding
// channels carry anything but silence produces a different number rather than a subtler failure.
//
// It also carries a default-active stereo side-chain input it never asks the host to disable,
// copying what a JUCE-wrapped plugin does (ZL Equalizer 2 was the specimen). That combination --
// an auxiliary bus the host does not drive but the plugin still reports as connected -- is what
// catches a host handing out null channel pointers, so the side-chain is summed into the output
// on every block. Silence from the host changes nothing; a null pointer stamps kNullBusStamp
// into the output instead of being dereferenced, so the failure is a wrong number and not a
// dead test process.
//
// A *third* class, `AIP Nameless Bus Plugin`, is the wide plugin one step meaner: besides
// insisting on its own width it declines `getBusArrangement` on the output bus outright, while
// `getBusInfo` reports the width as usual. That is what a JUCE plugin whose bus is an
// `AudioChannelSet::discreteChannels(n)`, n > 1, does -- no set of VST3 speaker bits denotes n
// positionless channels, so its wrapper has nothing to answer with -- and a host that reads the
// unanswered call as a refusal reports a working plugin as unloadable. Its busses are
// deliberately asymmetric (stereo in, fifteen out) because the specimen's were, and because a
// host that assumed the two agreed would size one of them from the other.

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/base/fstrdefs.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/main/pluginfactory.h"

#include <atomic>
#include <cmath>
#include <vector>

#define AIP_TEST_PLUGIN_NAME "AIP Test Plugin"
#define AIP_WIDE_PLUGIN_NAME "AIP Wide Plugin"
#define AIP_NAMELESS_BUS_PLUGIN_NAME "AIP Nameless Bus Plugin"
#define AIP_TEST_PLUGIN_VENDOR "audio-ipc2"

namespace aip::testplugin {

using namespace Steinberg;
using namespace Steinberg::Vst;

enum ParamIds : ParamID {
    kGainId = 0,
    kEditsId = 1,
    kMeterId = 2,
    kOffsetId = 3,
    kLatencyId = 4,
    kRestartId = 5,
    kEchoId = 6,
};

/// The gain a normalized value of 1.0 maps to. 0.5 is therefore unity, which keeps "did the host
/// wire the buffers up correctly" and "did the host apply the gain" separable in a test.
constexpr double kMaxGain = 2.0;

/// performEdit calls per block at a normalized `Edits` of 1.0.
constexpr int32 kMaxEditsPerBlock = 8;

/// Reported latency at a normalized `Latency` of 1.0. A round number of samples rather than a
/// plausible one: a test asserting on 256 is asserting on arithmetic it can do itself.
constexpr int32 kMaxLatencySamples = 512;

/// `Restart` maps [0, 1] onto [0, this], so the flag word F is requested with F / span. Covers
/// every RestartFlags bit the SDK defines, up to and including `kKeyswitchChanged` (1 << 10),
/// because the flags this host ignores need testing as much as the ones it obeys.
constexpr int32 kRestartFlagSpan = 2047;

/// Above this the plugin refuses the bus arrangement. Gives the host tests a deterministic
/// refusal to check against, which is otherwise hard to arrange.
constexpr int32 kMaxSupportedChannels = 8;

/// Written to the output when the host hands us a bus that claims channels but has a null
/// buffer. Chosen to be impossible to reach by arithmetic on any test signal.
constexpr float kNullBusStamp = -12345.0f;

/// Leads every state blob. A plugin that reads someone else's state and carries on regardless is
/// a plugin that restores garbage silently, so this one checks before it trusts.
constexpr int32 kStateMagic = 0x41495031; // "AIP1"

/// Gain, Edits and Offset, in that order. Meter is read-only and derived, so saving it would be
/// saving a measurement.
constexpr int32 kStateParameterCount = 3;

class TestPlugin : public SingleComponentEffect {
public:
    static const FUID cid;

    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new TestPlugin()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) {
            return result;
        }

        // Mono by default, though it will take anything up to kMaxSupportedChannels. The
        // default is deliberately *not* what the tests run at: a plugin whose declared
        // arrangement differs from the stream's is the only kind that can tell a host which
        // asked for what it wanted from a host which merely accepted what it was offered.
        addAudioInput(STR16("Input"), SpeakerArr::kMono);
        // Default-active, and never disabled below. See the note at the top of the file.
        addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, kAux);
        addAudioOutput(STR16("Output"), SpeakerArr::kMono);

        parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5, ParameterInfo::kCanAutomate, kGainId);
        parameters.addParameter(STR16("Edits"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate, kEditsId);
        parameters.addParameter(STR16("Meter"), nullptr, 0, 0.0, ParameterInfo::kIsReadOnly, kMeterId);
        parameters.addParameter(STR16("Offset"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate, kOffsetId);
        parameters.addParameter(STR16("Latency"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate, kLatencyId);
        parameters.addParameter(STR16("Restart"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate, kRestartId);
        parameters.addParameter(STR16("Echo"), nullptr, 0, 0.0, ParameterInfo::kCanAutomate, kEchoId);
        return kResultOk;
    }

    // What the host is allowed to believe, and only after a reactivation. See the note about
    // Latency at the top of the file.
    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return latencySamples.load(std::memory_order_relaxed); }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::setActive(state);
        if (result == kResultOk && state != 0) {
            // The moment the SDK names as the one after which the new figure is readable.
            latencySamples.store(pendingLatencySamples.load(std::memory_order_relaxed), std::memory_order_relaxed);
            if (echoNormalized.load(std::memory_order_relaxed) > 0.5 && componentHandler != nullptr) {
                // The pathological plugin: it tells the host about its latency from inside the
                // very reactivation the host performed to learn it. See the note at the top.
                componentHandler->restartComponent(kLatencyChanged);
            }
        }
        return result;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE {
        if (numOuts != 1 || numIns < 1 || numIns > 2 || inputs[0] != outputs[0]) {
            return kResultFalse;
        }
        if (SpeakerArr::getChannelCount(inputs[0]) > kMaxSupportedChannels) {
            return kResultFalse;
        }
        // The host asks for the side-chain to be kEmpty; we accept the call and keep it stereo
        // anyway. That is not obstinacy -- it is what the real plugin did, and a fixture that
        // quietly collapsed the bus would stop covering the case the host has to survive.
        SpeakerArrangement adjusted[2] = {inputs[0], SpeakerArr::kStereo};
        return SingleComponentEffect::setBusArrangements(adjusted, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API setProcessing(TBool state) SMTG_OVERRIDE {
        processing = state != 0;
        return kResultOk;
    }

    // A single component is its own controller, so this is how a host sets a value outside a
    // block -- and it is the only reason the processing members are atomic: the control thread
    // writes them while the audio thread reads them.
    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::setParamNormalized(tag, value);
        if (result != kResultOk) {
            return result;
        }
        if (tag == kGainId) {
            gainNormalized.store(value, std::memory_order_relaxed);
        } else if (tag == kEditsId) {
            editsNormalized.store(value, std::memory_order_relaxed);
        } else if (tag == kOffsetId) {
            offsetNormalized.store(value, std::memory_order_relaxed);
        } else if (tag == kLatencyId) {
            announceLatency(value);
        } else if (tag == kRestartId) {
            requestRestart(value);
        } else if (tag == kEchoId) {
            echoNormalized.store(value, std::memory_order_relaxed);
        }
        return result;
    }

    // IComponent's setState/getState, not IEditController's. SingleComponentEffect renames the
    // controller pair to set/getEditorState behind a macro precisely because the two clash, so
    // overriding these names here reaches the component half and only the component half -- which
    // is the half a host asks for when it saves a session.
    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kInvalidArgument;
        }

        int32 magic = 0;
        int32 read = 0;
        if (state->read(&magic, sizeof(magic), &read) != kResultOk || read != sizeof(magic) || magic != kStateMagic) {
            return kResultFalse;
        }

        double values[kStateParameterCount] = {};
        if (state->read(values, sizeof(values), &read) != kResultOk || read != sizeof(values)) {
            return kResultFalse;
        }

        setParamNormalized(kGainId, values[0]);
        setParamNormalized(kEditsId, values[1]);
        setParamNormalized(kOffsetId, values[2]);
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE {
        if (state == nullptr) {
            return kInvalidArgument;
        }

        int32 magic = kStateMagic;
        double values[kStateParameterCount] = {
            getParamNormalized(kGainId), getParamNormalized(kEditsId), getParamNormalized(kOffsetId)};
        int32 written = 0;
        if (state->write(&magic, sizeof(magic), &written) != kResultOk || written != sizeof(magic)) {
            return kResultFalse;
        }
        if (state->write(values, sizeof(values), &written) != kResultOk || written != sizeof(values)) {
            return kResultFalse;
        }
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        // Allocates on the first call and never again, which is what a great many real plugins do
        // and what the host's warm-up exists to absorb (PluginInstance::warmUp).
        //
        // The host cannot *count* this -- its detector replaces `operator new` per image and this
        // is a DLL with its own -- so no test asserts on it. It is here so the fixture has the
        // first-call laziness the warm-up is aimed at, rather than being a plugin for which the
        // warm-up is trivially a no-op.
        if (!firstProcessDone) {
            firstProcessDone = true;
            std::vector<float> lazy(64, 0.f);
            firstProcessSum = lazy.size();
        }

        applyInputParameterChanges(data);

        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) {
            return kResultOk;
        }

        const auto gain = static_cast<float>(gainNormalized.load(std::memory_order_relaxed) * kMaxGain);
        const auto offset = static_cast<float>(offsetNormalized.load(std::memory_order_relaxed));
        const int32 channels = data.inputs[0].numChannels < data.outputs[0].numChannels ? data.inputs[0].numChannels
                                                                                        : data.outputs[0].numChannels;

        float first = 0.f;
        for (int32 c = 0; c < channels; ++c) {
            const Sample32* in = data.inputs[0].channelBuffers32[c];
            Sample32* out = data.outputs[0].channelBuffers32[c];
            for (int32 s = 0; s < data.numSamples; ++s) {
                out[s] = in[s] * gain + offset;
            }
            if (c == 0 && data.numSamples > 0) {
                first = out[0];
            }
        }

        sumSideChain(data, channels);
        issueAudioThreadEdits(first);
        return kResultOk;
    }

private:
    // Adds every auxiliary input bus into the output. A host that does not drive the bus owes us
    // a well-formed silent buffer, so this must be a no-op; anything else is a host defect and
    // shows up as a wrong sample rather than as a crash.
    void sumSideChain(ProcessData& data, int32 channels) {
        for (int32 bus = 1; bus < data.numInputs; ++bus) {
            const AudioBusBuffers& aux = data.inputs[bus];
            const int32 auxChannels = aux.numChannels < channels ? aux.numChannels : channels;
            for (int32 c = 0; c < auxChannels; ++c) {
                Sample32* out = data.outputs[0].channelBuffers32[c];
                const Sample32* side = aux.channelBuffers32 != nullptr ? aux.channelBuffers32[c] : nullptr;
                if (side == nullptr) {
                    for (int32 s = 0; s < data.numSamples; ++s) {
                        out[s] = kNullBusStamp;
                    }
                    continue;
                }
                for (int32 s = 0; s < data.numSamples; ++s) {
                    out[s] += side[s];
                }
            }
        }
    }

    void applyInputParameterChanges(ProcessData& data) {
        if (data.inputParameterChanges == nullptr) {
            return;
        }
        const int32 count = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (queue == nullptr || queue->getPointCount() == 0) {
                continue;
            }
            int32 offset = 0;
            ParamValue value = 0.0;
            if (queue->getPoint(queue->getPointCount() - 1, offset, value) != kResultTrue) {
                continue;
            }
            switch (queue->getParameterId()) {
            case kGainId:
                gainNormalized.store(value, std::memory_order_relaxed);
                break;
            case kEditsId:
                editsNormalized.store(value, std::memory_order_relaxed);
                break;
            case kOffsetId:
                offsetNormalized.store(value, std::memory_order_relaxed);
                break;
            case kLatencyId:
                // Deliberately reachable from here as well as from setParamNormalized: a restart
                // request raised on the processing thread takes the lock-free ring rather than the
                // control-plane vector, and a host that only drains one of the two would pass
                // every control-thread test and drop this.
                announceLatency(value);
                break;
            case kRestartId:
                requestRestart(value);
                break;
            default:
                break;
            }
        }
    }

    // The point of the whole fixture: IComponentHandler, reached from the processing thread.
    void issueAudioThreadEdits(float reportedValue) {
        if (componentHandler == nullptr) {
            return;
        }
        const auto edits =
            static_cast<int32>(std::lround(editsNormalized.load(std::memory_order_relaxed) * kMaxEditsPerBlock));
        for (int32 i = 0; i < edits; ++i) {
            componentHandler->performEdit(kMeterId, static_cast<ParamValue>(reportedValue));
        }
    }

    /// Latency the plugin has decided on, which the host may not know about yet, and the figure
    /// `getLatencySamples` is allowed to return. Edge-triggered: setting `Latency` to the value it
    /// already holds announces nothing, because a plugin that re-announced an unchanged latency on
    /// every parameter set would make a host that obeys it rebuild forever.
    void announceLatency(ParamValue value) {
        const ParamValue previous = latencyNormalized.exchange(value, std::memory_order_relaxed);
        if (previous == value) {
            return;
        }
        pendingLatencySamples.store(
            static_cast<uint32>(std::lround(value * kMaxLatencySamples)), std::memory_order_relaxed);
        if (componentHandler != nullptr) {
            componentHandler->restartComponent(kLatencyChanged);
        }
    }

    /// Raises whatever flag word `Restart` encodes, once per change of the value. Zero raises
    /// nothing, which is what makes 0.0 a usable default.
    void requestRestart(ParamValue value) {
        const ParamValue previous = restartNormalized.exchange(value, std::memory_order_relaxed);
        if (previous == value || componentHandler == nullptr) {
            return;
        }
        const auto flags = static_cast<int32>(std::lround(value * kRestartFlagSpan));
        if (flags == 0) {
            return;
        }
        componentHandler->restartComponent(flags);
    }

    /// See the top of `process`. Not atomic: written once, on whichever thread gets there first,
    /// and read by nothing that matters.
    bool firstProcessDone = false;
    std::size_t firstProcessSum = 0;

    std::atomic<ParamValue> gainNormalized{0.5};
    std::atomic<ParamValue> editsNormalized{0.0};
    std::atomic<ParamValue> offsetNormalized{0.0};
    std::atomic<ParamValue> latencyNormalized{0.0};
    std::atomic<ParamValue> restartNormalized{0.0};
    std::atomic<ParamValue> echoNormalized{0.0};
    std::atomic<uint32> pendingLatencySamples{0};
    std::atomic<uint32> latencySamples{0};
    bool processing = false;
};

const FUID TestPlugin::cid(0xA1B2C301, 0x4E5F6071, 0x82934455, 0x66778899);

/// The only width `WidePlugin` will discuss.
constexpr int32 kWideChannels = 8;

/// A plugin with one fixed, wide bus and no opinion about anything else.
///
/// Every channel of the output is its own input channel plus the sum of the *whole* input bus.
/// That is deliberately sensitive to the padding: at a stereo stream the host pads six channels
/// with silence, so the sum is the two real channels and nothing else -- and if any of those six
/// carried leftovers from a previous plugin instead, every output channel would say so. A chain
/// of two of these is therefore a direct test of whether the host re-zeroes the padding between
/// plugins or merely filled it once.
///
/// No parameters and no state: the point of this class is the negotiation, and everything else is
/// already covered by TestPlugin.
class WidePlugin : public SingleComponentEffect {
public:
    static const FUID cid;

    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new WidePlugin()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) {
            return result;
        }
        addAudioInput(STR16("Input"), SpeakerArr::k71Cine);
        addAudioOutput(STR16("Output"), SpeakerArr::k71Cine);
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    // Eight channels or nothing. Which eight is not our business -- the host is free to name them
    // whatever the endpoint calls them -- but the count is not negotiable, and a refusal here
    // leaves the bus at the k71Cine it was created with, which is what the host reads back.
    tresult PLUGIN_API setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1 || inputs[0] != outputs[0] ||
            SpeakerArr::getChannelCount(inputs[0]) != kWideChannels) {
            return kResultFalse;
        }
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) {
            return kResultOk;
        }
        const int32 inChannels = data.inputs[0].numChannels;
        const int32 outChannels = data.outputs[0].numChannels;

        for (int32 s = 0; s < data.numSamples; ++s) {
            Sample32 busSum = 0.f;
            for (int32 c = 0; c < inChannels; ++c) {
                busSum += data.inputs[0].channelBuffers32[c][s];
            }
            for (int32 c = 0; c < outChannels; ++c) {
                const Sample32 own = c < inChannels ? data.inputs[0].channelBuffers32[c][s] : Sample32{0.f};
                data.outputs[0].channelBuffers32[c][s] = own + busSum;
            }
        }
        return kResultOk;
    }
};

const FUID WidePlugin::cid(0xA1B2C302, 0x4E5F6071, 0x82934455, 0x66778899);

/// The width `NamelessBusPlugin` insists on, and its arrangement: the low fifteen speaker bits.
///
/// Fifteen because that is what the specimen has (soundscape_zone_demo_cu, a fifteen-speaker cabin
/// renderer) and because no standard VST3 layout has that many, so nothing here can pass by
/// accident on a lucky guess from `speakerArrangementFor`'s table of named layouts.
constexpr int32 kNamelessChannels = 15;
constexpr SpeakerArrangement kNamelessArrangement = (SpeakerArrangement{1} << kNamelessChannels) - 1;

/// A plugin whose output bus is wide, fixed, and *unnameable*: it will not tell a host what
/// arrangement it wants, while reporting the width perfectly well through `getBusInfo`.
///
/// Not a hypothetical. A JUCE bus declared `AudioChannelSet::discreteChannels(n)` for n > 1 is
/// exactly this: no combination of VST3 speaker bits denotes "n channels with no assigned
/// positions", so JUCE's wrapper fails `getBusArrangement` outright -- with a `jassertfalse` and a
/// comment saying it cannot represent the layout -- one call after `getBusInfo` said `n`. Every
/// plugin built that way lands here.
///
/// The two together are what makes it a host test rather than a plugin test. Tiers 1 and 2 are
/// refused, because the plugin's width is not the stream's; tier 3 then has to learn the width
/// from `getBusInfo` and guess an arrangement of it, because the call that would have named one
/// does not answer. A host that treats the unanswered call as a refusal reports a working plugin
/// as unloadable, which is the bug this class exists to keep fixed.
///
/// The input bus is an ordinary stereo one and names itself normally: the specimen's does, and a
/// fixture that failed both directions could not tell a host that fell back on both from one that
/// fell back everywhere.
///
/// DSP is WidePlugin's, for WidePlugin's reason -- every output channel is its own input plus the
/// sum of the whole input bus, so padding that is not silence shows up as a wrong number.
class NamelessBusPlugin : public SingleComponentEffect {
public:
    static const FUID cid;

    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new NamelessBusPlugin()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) {
            return result;
        }
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Speaker Output"), kNamelessArrangement);
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    // Stereo in, fifteen out, and no other shape. Which fifteen is not our business; the count is.
    tresult PLUGIN_API setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1 || SpeakerArr::getChannelCount(inputs[0]) != 2 ||
            SpeakerArr::getChannelCount(outputs[0]) != kNamelessChannels) {
            return kResultFalse;
        }
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    // The whole point. The output bus has no name for its layout, so the honest answer is that
    // there is no answer -- `getBusInfo` still reports fifteen channels, because the base class
    // took the arrangement above at face value and a width is not a layout.
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) SMTG_OVERRIDE {
        if (dir == kOutput) {
            return kResultFalse;
        }
        return SingleComponentEffect::getBusArrangement(dir, index, arr);
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) {
            return kResultOk;
        }
        const int32 inChannels = data.inputs[0].numChannels;
        const int32 outChannels = data.outputs[0].numChannels;

        for (int32 s = 0; s < data.numSamples; ++s) {
            Sample32 busSum = 0.f;
            for (int32 c = 0; c < inChannels; ++c) {
                busSum += data.inputs[0].channelBuffers32[c][s];
            }
            for (int32 c = 0; c < outChannels; ++c) {
                const Sample32 own = c < inChannels ? data.inputs[0].channelBuffers32[c][s] : Sample32{0.f};
                data.outputs[0].channelBuffers32[c][s] = own + busSum;
            }
        }
        return kResultOk;
    }
};

const FUID NamelessBusPlugin::cid(0xA1B2C303, 0x4E5F6071, 0x82934455, 0x66778899);

} // namespace aip::testplugin

BEGIN_FACTORY_DEF(AIP_TEST_PLUGIN_VENDOR, "https://example.invalid", "mailto:nobody@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(aip::testplugin::TestPlugin::cid), PClassInfo::kManyInstances, kVstAudioEffectClass,
    AIP_TEST_PLUGIN_NAME, 0, Steinberg::Vst::PlugType::kFx, "1.0.0", kVstVersionString,
    aip::testplugin::TestPlugin::createInstance)

// Second, and second on purpose: `Engine::appendPlugin` takes a module's *first* audio-effect
// class, so every existing test goes on getting TestPlugin without saying so.
DEF_CLASS2(INLINE_UID_FROM_FUID(aip::testplugin::WidePlugin::cid), PClassInfo::kManyInstances, kVstAudioEffectClass,
    AIP_WIDE_PLUGIN_NAME, 0, Steinberg::Vst::PlugType::kFx, "1.0.0", kVstVersionString,
    aip::testplugin::WidePlugin::createInstance)

// Third, for the same reason the second is second.
DEF_CLASS2(INLINE_UID_FROM_FUID(aip::testplugin::NamelessBusPlugin::cid), PClassInfo::kManyInstances,
    kVstAudioEffectClass, AIP_NAMELESS_BUS_PLUGIN_NAME, 0, Steinberg::Vst::PlugType::kFx, "1.0.0", kVstVersionString,
    aip::testplugin::NamelessBusPlugin::createInstance)

END_FACTORY
