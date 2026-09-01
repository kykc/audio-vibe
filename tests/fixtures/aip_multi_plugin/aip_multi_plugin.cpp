// A VST3 module shaped like `lsp-plugins.vst3`: a mono effect first, a stereo one after it.
//
// It exists because a `.vst3` is a *module*, not a plugin, and this project got that wrong in
// every place that took a bundle path without a class beside it. The specimen is LSP, whose
// bundle exposes `Impulse Responses Mono` before `Impulse Responses Stereo` -- and whose full
// distribution ships dozens of effects in one file. A host that resolves a path to the module's
// first audio effect gets the mono one, which then refuses a stereo stream with "accepts at most
// 1 channels", and the stereo effect sitting in the same file is unreachable by path alone.
//
// `aip_test_plugin` cannot stand in for this. Its first class takes a stereo stream perfectly
// well, so nothing about it distinguishes a host that picks the first class from one that picks
// the first *workable* class -- and reordering it would silently repoint sixty tests that say
// `appendPlugin` and mean `AIP Test Plugin`. Hence a second module, whose whole content is the
// ordering: the class that must be skipped is first, and the class that must be found is not.
//
// Same hand-built bundle layout as the other fixtures, and for the same reasons -- see
// `aip_test_plugin/CMakeLists.txt` for why `smtg_add_vst3plugin` is not used.
//
// Neither class has parameters or state. Everything about a plugin except which stream widths it
// will accept is already covered by `aip_test_plugin`, and a fixture that repeated it would be a
// second thing to keep in step for no extra coverage. The one behaviour they do have is
// arithmetic that tells them apart: the mono class passes its input through unchanged, the stereo
// class doubles it. A test can therefore assert which of the two a host chose from the samples
// alone, without trusting the name the host reports.

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/base/fstrdefs.h"
#include "public.sdk/source/main/pluginfactory.h"

#include <algorithm>

#define AIP_MONO_ONLY_PLUGIN_NAME "AIP Mono Only Plugin"
#define AIP_MULTI_STEREO_PLUGIN_NAME "AIP Multi Stereo Plugin"
#define AIP_MULTI_PLUGIN_VENDOR "audio-ipc2"

namespace aip::multiplugin {

using namespace Steinberg;
using namespace Steinberg::Vst;

/// What `MultiStereoPlugin` will widen to. Matches `aip_test_plugin`'s ceiling so that a test at
/// any ordinary endpoint width gets the same answer.
constexpr int32 kMaxSupportedChannels = 8;

/// The gain `MultiStereoPlugin` applies, so that which class a host chose is visible in the
/// samples and not only in the name it reports.
constexpr Sample32 kStereoGain = 2.0f;

/// Mono and nothing else, which is the whole point of it.
///
/// It declares mono busses and refuses every arrangement that is not mono, so a host asking for a
/// stereo stream is refused at tiers 1 and 2 and then reads one channel back at tier 3 -- narrower
/// than the stream, which `PluginInstance::prepare` will not pad down to. The refusal it produces
/// is verbatim the one LSP's mono effect produces.
class MonoOnlyPlugin : public SingleComponentEffect {
public:
    static const FUID cid;

    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new MonoOnlyPlugin()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) {
            return result;
        }
        addAudioInput(STR16("Input"), SpeakerArr::kMono);
        addAudioOutput(STR16("Output"), SpeakerArr::kMono);
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1 || SpeakerArr::getChannelCount(inputs[0]) != 1 ||
            SpeakerArr::getChannelCount(outputs[0]) != 1) {
            return kResultFalse;
        }
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) {
            return kResultOk;
        }
        const int32 channels = std::min(data.inputs[0].numChannels, data.outputs[0].numChannels);
        for (int32 c = 0; c < channels; ++c) {
            for (int32 s = 0; s < data.numSamples; ++s) {
                data.outputs[0].channelBuffers32[c][s] = data.inputs[0].channelBuffers32[c][s];
            }
        }
        return kResultOk;
    }
};

const FUID MonoOnlyPlugin::cid(0xA1B2C401, 0x4E5F6071, 0x82934455, 0x66778899);

/// The ordinary effect the module exists to make reachable. Stereo by default and content with
/// anything up to eight channels, so it takes whatever an endpoint carries.
class MultiStereoPlugin : public SingleComponentEffect {
public:
    static const FUID cid;

    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new MultiStereoPlugin()); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) {
            return result;
        }
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API setBusArrangements(
        SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE {
        if (numIns != 1 || numOuts != 1 || inputs[0] != outputs[0] ||
            SpeakerArr::getChannelCount(inputs[0]) > kMaxSupportedChannels) {
            return kResultFalse;
        }
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) {
            return kResultOk;
        }
        const int32 channels = std::min(data.inputs[0].numChannels, data.outputs[0].numChannels);
        for (int32 c = 0; c < channels; ++c) {
            for (int32 s = 0; s < data.numSamples; ++s) {
                data.outputs[0].channelBuffers32[c][s] = data.inputs[0].channelBuffers32[c][s] * kStereoGain;
            }
        }
        return kResultOk;
    }
};

const FUID MultiStereoPlugin::cid(0xA1B2C402, 0x4E5F6071, 0x82934455, 0x66778899);

} // namespace aip::multiplugin

BEGIN_FACTORY_DEF(AIP_MULTI_PLUGIN_VENDOR, "https://example.invalid", "mailto:nobody@example.invalid")

// First, and first on purpose: this is the class a host resolving a bundle path to "the first
// audio effect" would get, and the one it must skip on a stereo stream.
DEF_CLASS2(INLINE_UID_FROM_FUID(aip::multiplugin::MonoOnlyPlugin::cid), PClassInfo::kManyInstances,
    kVstAudioEffectClass, AIP_MONO_ONLY_PLUGIN_NAME, 0, Steinberg::Vst::PlugType::kFx, "1.0.0", kVstVersionString,
    aip::multiplugin::MonoOnlyPlugin::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(aip::multiplugin::MultiStereoPlugin::cid), PClassInfo::kManyInstances,
    kVstAudioEffectClass, AIP_MULTI_STEREO_PLUGIN_NAME, 0, Steinberg::Vst::PlugType::kFx, "1.0.0", kVstVersionString,
    aip::multiplugin::MultiStereoPlugin::createInstance)

END_FACTORY
