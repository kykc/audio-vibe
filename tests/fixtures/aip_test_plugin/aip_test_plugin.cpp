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

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/base/fstrdefs.h"
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
};

/// The gain a normalized value of 1.0 maps to. 0.5 is therefore unity, which keeps "did the host
/// wire the buffers up correctly" and "did the host apply the gain" separable in a test.
constexpr double kMaxGain = 2.0;

/// performEdit calls per block at a normalized `Edits` of 1.0.
constexpr int32 kMaxEditsPerBlock = 8;

/// Above this the plugin refuses the bus arrangement. Gives the host tests a deterministic
/// refusal to check against, which is otherwise hard to arrange.
constexpr int32 kMaxSupportedChannels = 8;

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
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);

        parameters.addParameter(STR16("Gain"), nullptr, 0, 0.5,
                                ParameterInfo::kCanAutomate, kGainId);
        parameters.addParameter(STR16("Edits"), nullptr, 0, 0.0,
                                ParameterInfo::kCanAutomate, kEditsId);
        parameters.addParameter(STR16("Meter"), nullptr, 0, 0.0,
                                ParameterInfo::kIsReadOnly, kMeterId);
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns,
                                          SpeakerArrangement* outputs,
                                          int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1 || inputs[0] != outputs[0]) {
            return kResultFalse;
        }
        if (SpeakerArr::getChannelCount(inputs[0]) > kMaxSupportedChannels) {
            return kResultFalse;
        }
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
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
        }
        return result;
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        applyInputParameterChanges(data);

        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) {
            return kResultOk;
        }

        const auto gain =
            static_cast<float>(gainNormalized.load(std::memory_order_relaxed) * kMaxGain);
        const int32 channels = data.inputs[0].numChannels < data.outputs[0].numChannels
                                   ? data.inputs[0].numChannels
                                   : data.outputs[0].numChannels;

        float first = 0.f;
        for (int32 c = 0; c < channels; ++c) {
            const Sample32* in = data.inputs[0].channelBuffers32[c];
            Sample32* out = data.outputs[0].channelBuffers32[c];
            for (int32 s = 0; s < data.numSamples; ++s) {
                out[s] = in[s] * gain;
            }
            if (c == 0 && data.numSamples > 0) {
                first = out[0];
            }
        }

        issueAudioThreadEdits(first);
        return kResultOk;
    }

private:
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
