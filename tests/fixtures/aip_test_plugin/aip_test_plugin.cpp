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
//   Gain   (id 0)  output = input * (normalized * 2), so the 0.5 default is unity
//   Edits  (id 1)  performEdit calls per process() = round(normalized * 8), from the audio thread
//   Meter  (id 2)  the parameter those callbacks report; its value is the block's first sample
//   Offset (id 3)  a constant added *after* the gain; 0 by default
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
// It also carries a default-active stereo side-chain input it never asks the host to disable,
// copying what a JUCE-wrapped plugin does (ZL Equalizer 2 was the specimen). That combination --
// an auxiliary bus the host does not drive but the plugin still reports as connected -- is what
// catches a host handing out null channel pointers, so the side-chain is summed into the output
// on every block. Silence from the host changes nothing; a null pointer stamps kNullBusStamp
// into the output instead of being dereferenced, so the failure is a wrong number and not a
// dead test process.

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/base/fstrdefs.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/main/pluginfactory.h"

#include <atomic>
#include <cmath>

#define AIP_TEST_PLUGIN_NAME "AIP Test Plugin"
#define AIP_TEST_PLUGIN_VENDOR "audio-ipc2"

namespace aip::testplugin {

using namespace Steinberg;
using namespace Steinberg::Vst;

enum ParamIds : ParamID {
    kGainId = 0,
    kEditsId = 1,
    kMeterId = 2,
    kOffsetId = 3,
};

/// The gain a normalized value of 1.0 maps to. 0.5 is therefore unity, which keeps "did the host
/// wire the buffers up correctly" and "did the host apply the gain" separable in a test.
constexpr double kMaxGain = 2.0;

/// performEdit calls per block at a normalized `Edits` of 1.0.
constexpr int32 kMaxEditsPerBlock = 8;

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

    static FUnknown* createInstance(void*) {
        return static_cast<IAudioProcessor*>(new TestPlugin());
    }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) {
            return result;
        }

        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        // Default-active, and never disabled below. See the note at the top of the file.
        addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, kAux);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);

        parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5,
                                ParameterInfo::kCanAutomate, kGainId);
        parameters.addParameter(STR16("Edits"), nullptr, 0, 0.0,
                                ParameterInfo::kCanAutomate, kEditsId);
        parameters.addParameter(STR16("Meter"), nullptr, 0, 0.0,
                                ParameterInfo::kIsReadOnly, kMeterId);
        parameters.addParameter(STR16("Offset"), nullptr, 0, 0.0,
                                ParameterInfo::kCanAutomate, kOffsetId);
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                          SpeakerArrangement* outputs,
                                          int32 numOuts) SMTG_OVERRIDE {
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
        if (state->read(&magic, sizeof(magic), &read) != kResultOk || read != sizeof(magic) ||
            magic != kStateMagic) {
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
        double values[kStateParameterCount] = {getParamNormalized(kGainId),
                                               getParamNormalized(kEditsId),
                                               getParamNormalized(kOffsetId)};
        int32 written = 0;
        if (state->write(&magic, sizeof(magic), &written) != kResultOk ||
            written != sizeof(magic)) {
            return kResultFalse;
        }
        if (state->write(values, sizeof(values), &written) != kResultOk ||
            written != sizeof(values)) {
            return kResultFalse;
        }
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        applyInputParameterChanges(data);

        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) {
            return kResultOk;
        }

        const auto gain =
            static_cast<float>(gainNormalized.load(std::memory_order_relaxed) * kMaxGain);
        const auto offset =
            static_cast<float>(offsetNormalized.load(std::memory_order_relaxed));
        const int32 channels = data.inputs[0].numChannels < data.outputs[0].numChannels
                                   ? data.inputs[0].numChannels
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
                const Sample32* side =
                    aux.channelBuffers32 != nullptr ? aux.channelBuffers32[c] : nullptr;
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
        const auto edits = static_cast<int32>(
            std::lround(editsNormalized.load(std::memory_order_relaxed) * kMaxEditsPerBlock));
        for (int32 i = 0; i < edits; ++i) {
            componentHandler->performEdit(kMeterId, static_cast<ParamValue>(reportedValue));
        }
    }

    std::atomic<ParamValue> gainNormalized{0.5};
    std::atomic<ParamValue> editsNormalized{0.0};
    std::atomic<ParamValue> offsetNormalized{0.0};
    bool processing = false;
};

const FUID TestPlugin::cid(0xA1B2C301, 0x4E5F6071, 0x82934455, 0x66778899);

} // namespace aip::testplugin

BEGIN_FACTORY_DEF(AIP_TEST_PLUGIN_VENDOR, "https://example.invalid",
                  "mailto:nobody@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(aip::testplugin::TestPlugin::cid), PClassInfo::kManyInstances,
           kVstAudioEffectClass, AIP_TEST_PLUGIN_NAME, 0, Steinberg::Vst::PlugType::kFx, "1.0.0",
           kVstVersionString, aip::testplugin::TestPlugin::createInstance)

END_FACTORY
