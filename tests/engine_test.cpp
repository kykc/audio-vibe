// The VST3 host, driven end to end (design_doc.md sec. 7.4.3, sec. 7.4.5).
//
// Every test here runs a real VST3 module -- `tests/fixtures/aip_test_plugin` -- through the real
// valet thread, fed by the synthetic king. Nothing is mocked below the protocol, because the
// interesting failures (buffers wired to the wrong channel, a plugin allocating on the audio
// thread, a chain freed while it is being read) only appear when all three are running together.
//
// The fixture's contract, restated so the expected numbers below can be read without opening it:
//
//   Gain   (id 0)  output = input * (normalized * 2); the 0.5 default is unity
//   Edits  (id 1)  performEdit calls per process(), from the audio thread = round(normalized * 8)
//   Offset (id 3)  a constant added after the gain, so two instances do not commute
//   it refuses any bus arrangement above 8 channels, and carries a default-active side-chain

#include "harness/synthetic_king.h"
#include "harness/wait_for.h"

#include "aip/engine/engine.h"
#include "aip/engine/plugin_instance.h"
#include "aip/ipc/buffer_valet.h"
#include "aip/ipc/valet_thread.h"
#include "aip/rt/realtime_guard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <string>
#include <vector>

using namespace aip;
using Catch::Approx;

namespace {

/// Written by CMake; there is no way for the test to work the bundle path out at run time.
const std::string kTestPluginPath = AIP_TEST_PLUGIN_PATH;

constexpr Steinberg::Vst::ParamID kGainParam = 0;
constexpr Steinberg::Vst::ParamID kEditsParam = 1;
constexpr Steinberg::Vst::ParamID kOffsetParam = 3;

engine::StreamFormat stereoFormat(std::uint32_t sampleRate = 48000) {
    return engine::StreamFormat{sampleRate, 2, engine::kDefaultMaxFrames};
}

/// One attached valet, its thread, and the king feeding it -- the whole rig every test below
/// needs, torn down in the right order.
class Rig {
public:
    Rig(ipc::BlockProcessor& processor, std::wstring_view tag, std::uint32_t sampleRate = 48000,
        std::uint32_t channelCount = 2)
        : base_(harness::uniqueTestObjectBase(tag)), king_(base_) {
        REQUIRE(king_.open(sampleRate, channelCount));
        REQUIRE(valet_.attach(base_));
        thread_ = std::make_unique<ipc::ValetThread>(valet_, processor, counters_);
        thread_->start();
    }

    ~Rig() {
        thread_->stop();
        valet_.detach();
    }

    harness::SyntheticKing& king() noexcept { return king_; }

    ipc::ValetCounters& counters() noexcept { return counters_; }

    /// Publishes one interleaved block and returns what came back, also interleaved.
    std::vector<float> run(const std::vector<float>& in) {
        std::vector<float> out(in.size(), 0.f);
        REQUIRE(king_.dispatch(in.data(), out.data(), static_cast<std::int32_t>(in.size())) ==
                harness::DispatchResult::Processed);
        return out;
    }

private:
    std::wstring base_;
    harness::SyntheticKing king_;
    ipc::BufferValet valet_;
    ipc::ValetCounters counters_;
    std::unique_ptr<ipc::ValetThread> thread_;
};

/// Interleaved stereo ramp. Channel 0 counts up from 1, channel 1 counts down from -1, so a
/// channel swap anywhere in the planar round trip shows up as a sign flip rather than as nothing.
std::vector<float> signedRamp(std::int32_t frames) {
    std::vector<float> samples(static_cast<std::size_t>(frames) * 2);
    for (std::int32_t f = 0; f < frames; ++f) {
        samples[static_cast<std::size_t>(f) * 2] = static_cast<float>(f + 1);
        samples[static_cast<std::size_t>(f) * 2 + 1] = -static_cast<float>(f + 1);
    }
    return samples;
}

/// Reaches the plugin through the *rack*, not through the published chain. That is the whole
/// point of the split: a rack position is stable across rebuilds, and a chain index is not.
void setParameter(engine::Engine& host, std::size_t pluginIndex, Steinberg::Vst::ParamID id,
                  double normalized) {
    engine::PluginInstance* plugin = host.pluginAt(pluginIndex);
    REQUIRE(plugin != nullptr);
    Steinberg::Vst::IEditController* controller = plugin->controller();
    REQUIRE(controller != nullptr);
    REQUIRE(controller->setParamNormalized(id, normalized) == Steinberg::kResultOk);
}

double getParameter(engine::Engine& host, std::size_t pluginIndex, Steinberg::Vst::ParamID id) {
    engine::PluginInstance* plugin = host.pluginAt(pluginIndex);
    REQUIRE(plugin != nullptr);
    Steinberg::Vst::IEditController* controller = plugin->controller();
    REQUIRE(controller != nullptr);
    return controller->getParamNormalized(id);
}

/// The class id of the module's wide plugin, resolved rather than hardcoded: a UID written out
/// by hand is a test that fails for the wrong reason the day the fixture is edited.
std::string widePluginClassId() {
    std::string error;
    engine::PluginModule::Ptr module = engine::PluginModule::load(kTestPluginPath, error);
    REQUIRE(module != nullptr);
    REQUIRE(module->audioEffects().size() == 2);
    return module->audioEffects()[1].id.toString();
}

} // namespace

TEST_CASE("the test plugin module loads and exposes one audio effect", "[engine][module]") {
    std::string error;
    engine::PluginModule::Ptr module = engine::PluginModule::load(kTestPluginPath, error);

    INFO("path: " << kTestPluginPath << " error: " << error);
    REQUIRE(module != nullptr);
    CHECK(error.empty());
    REQUIRE(module->audioEffects().size() == 2);
    CHECK(module->audioEffects().front().name == "AIP Test Plugin");
    CHECK(module->audioEffects().front().vendor == "audio-ipc2");
    // Second, so that `appendPlugin` -- which takes the first audio effect -- goes on meaning
    // TestPlugin everywhere below.
    CHECK(module->audioEffects().back().name == "AIP Wide Plugin");
}

TEST_CASE("loading something that is not a plugin fails without throwing", "[engine][module]") {
    std::string error;
    engine::PluginModule::Ptr module =
        engine::PluginModule::load("D:/definitely/not/here.vst3", error);

    CHECK(module == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("a published chain processes real blocks through the valet thread", "[engine][chain]") {
    constexpr std::int32_t kFrames = 128;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    Rig rig(host.blockProcessor(), L"engine-chain");
    const std::vector<float> in = signedRamp(kFrames);

    SECTION("unity by default, and channels stay where they were") {
        const std::vector<float> out = rig.run(in);
        for (std::size_t i = 0; i < in.size(); ++i) {
            REQUIRE(out[i] == Approx(in[i]));
        }
    }

    SECTION("a gain change on the control thread reaches the audio thread") {
        setParameter(host, 0, kGainParam, 1.0); // normalized 1.0 -> 2x
        const std::vector<float> out = rig.run(in);
        for (std::size_t i = 0; i < in.size(); ++i) {
            REQUIRE(out[i] == Approx(in[i] * 2.f));
        }
    }

    CHECK(host.chainProcessor().blocksProcessed() >= 1);
    CHECK(host.chainProcessor().formatMismatches() == 0);
}

TEST_CASE("a side-chain bus is negotiated away and backed with silence", "[engine][chain]") {
    // The case a real plugin found: ZL Equalizer 2 carries a default-active stereo side-chain and
    // rejected a bus arrangement that described only the main pair. Describing every bus fixed
    // the refusal -- and then left a second hazard, because the plugin goes on reporting the
    // side-chain as two connected channels while the host drives nothing into it.
    //
    // The fixture reproduces both halves deliberately (see aip_test_plugin.cpp) and sums its
    // side-chain into the output, so an unchanged output is positive evidence that the host
    // handed it a real, zeroed buffer rather than a null pointer or leftover memory.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);

    // The plugin accepted a description of all its busses, side-chain included.
    CHECK(plugin->fullBusNegotiation());

    Steinberg::Vst::IComponent* component = plugin->component();
    REQUIRE(component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) == 2);

    Steinberg::Vst::BusInfo aux{};
    REQUIRE(component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 1, aux) ==
            Steinberg::kResultOk);
    CHECK(aux.busType == Steinberg::Vst::kAux);
    // Still claiming channels after the negotiation -- which is exactly why the silence backing
    // has to exist rather than being an optimisation.
    CHECK(aux.channelCount == 2);

    Rig rig(host.blockProcessor(), L"engine-sidechain");
    const std::vector<float> in = signedRamp(kFrames);
    setParameter(host, 0, kGainParam, 1.0);
    const std::vector<float> out = rig.run(in);

    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 2.f));
    }
}

TEST_CASE("two plugins compose in order", "[engine][chain]") {
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    REQUIRE(host.chainProcessor().current()->size() == 2);

    // 2x then 4x. Distinct factors, so a chain that ran one plugin twice, or ran them in the
    // wrong order into the wrong bank, would not land on 8x by accident.
    setParameter(host, 0, kGainParam, 1.0);
    setParameter(host, 1, kGainParam, 1.0);

    Rig rig(host.blockProcessor(), L"engine-two");
    const std::vector<float> in = signedRamp(kFrames);
    const std::vector<float> out = rig.run(in);

    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 4.f));
    }
}

TEST_CASE("rack mutation preserves the plugins it does not touch", "[engine][rack]") {
    // The property the whole rack/chain split exists for. A chain that owned its plugins would
    // reconstruct every one of them on each of these operations, so the first plugin's gain would
    // quietly revert the moment a second was added -- indistinguishable, from the outside, from a
    // UI bug.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    setParameter(host, 0, kGainParam, 1.0); // 2x

    Rig rig(host.blockProcessor(), L"engine-rack");
    const std::vector<float> in = signedRamp(kFrames);

    SECTION("adding a second plugin leaves the first alone") {
        REQUIRE(host.appendPlugin(kTestPluginPath, error));
        REQUIRE(host.pluginCount() == 2);

        // Plugin 0 is still at 2x; plugin 1 is new and therefore at its unity default.
        CHECK(getParameter(host, 0, kGainParam) == Approx(1.0));
        CHECK(getParameter(host, 1, kGainParam) == Approx(0.5));

        const std::vector<float> out = rig.run(in);
        for (std::size_t i = 0; i < in.size(); ++i) {
            REQUIRE(out[i] == Approx(in[i] * 2.f));
        }
    }

    SECTION("inserting before the existing plugin keeps rack positions meaningful") {
        REQUIRE(host.insertPlugin(0, kTestPluginPath, error));
        REQUIRE(host.pluginCount() == 2);

        // The plugin that was at 0 moved to 1 and kept its gain; the newcomer took position 0.
        CHECK(getParameter(host, 0, kGainParam) == Approx(0.5));
        CHECK(getParameter(host, 1, kGainParam) == Approx(1.0));
    }

    SECTION("removing a plugin destroys only that one") {
        REQUIRE(host.appendPlugin(kTestPluginPath, error));
        setParameter(host, 1, kGainParam, 1.0);
        REQUIRE(host.removePlugin(0));

        REQUIRE(host.pluginCount() == 1);
        CHECK(host.strandedPlugins() == 0);
        CHECK(getParameter(host, 0, kGainParam) == Approx(1.0));

        const std::vector<float> out = rig.run(in);
        for (std::size_t i = 0; i < in.size(); ++i) {
            REQUIRE(out[i] == Approx(in[i] * 2.f));
        }
    }

    SECTION("bypass takes the plugin out of the signal path and puts it back") {
        REQUIRE(host.setBypass(0, true));
        CHECK(host.bypassed(0));
        CHECK(host.pluginCount() == 1); // still in the rack, just not in the chain
        CHECK(host.chainProcessor().current()->size() == 0);

        const std::vector<float> bypassed = rig.run(in);
        CHECK(bypassed == in);

        REQUIRE(host.setBypass(0, false));
        CHECK_FALSE(host.bypassed(0));

        // Un-bypassing restores the plugin *and* the gain it had, because it was never destroyed.
        const std::vector<float> restored = rig.run(in);
        for (std::size_t i = 0; i < in.size(); ++i) {
            REQUIRE(restored[i] == Approx(in[i] * 2.f));
        }
    }
}

TEST_CASE("reordering the rack reorders processing", "[engine][rack]") {
    // Two gains would not do: multiplication commutes, so a rack that ran them backwards would
    // produce byte-identical output and this test would pass while proving nothing. The fixture's
    // Offset is applied after its Gain, which makes the pair non-commutative:
    //
    //   gain-then-offset : (x * 2) + 0.25
    //   offset-then-gain : (x + 0.25) * 2  =  2x + 0.5
    constexpr std::int32_t kFrames = 32;
    constexpr float kOffset = 0.25f;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error)); // rack 0: the gain
    REQUIRE(host.appendPlugin(kTestPluginPath, error)); // rack 1: the offset
    REQUIRE(host.rebuild(stereoFormat(), error));

    setParameter(host, 0, kGainParam, 1.0);                            // 2x, no offset
    setParameter(host, 1, kOffsetParam, static_cast<double>(kOffset)); // unity, +0.25

    Rig rig(host.blockProcessor(), L"engine-reorder");
    const std::vector<float> in = signedRamp(kFrames);

    const std::vector<float> gainFirst = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(gainFirst[i] == Approx(in[i] * 2.f + kOffset));
    }

    REQUIRE(host.movePlugin(0, 1));

    // The parameters travelled with their plugins rather than staying at their rack positions.
    CHECK(getParameter(host, 0, kOffsetParam) == Approx(kOffset));
    CHECK(getParameter(host, 1, kGainParam) == Approx(1.0));
    CHECK(host.chainProcessor().current()->size() == 2);

    const std::vector<float> offsetFirst = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(offsetFirst[i] == Approx((in[i] + kOffset) * 2.f));
    }
}

TEST_CASE("blocks pass through untouched when no chain is published", "[engine][chain]") {
    constexpr std::int32_t kFrames = 32;

    engine::Engine host; // nothing appended, nothing rebuilt
    Rig rig(host.blockProcessor(), L"engine-empty");

    const std::vector<float> in = signedRamp(kFrames);
    const std::vector<float> out = rig.run(in);

    CHECK(out == in);
    CHECK(host.chainProcessor().blocksPassedThrough() >= 1);
    CHECK(host.chainProcessor().blocksProcessed() == 0);
}

TEST_CASE("a format the chain was not built for is passed through and reported",
          "[engine][format]") {
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(44100), error));
    setParameter(host, 0, kGainParam, 1.0);

    // The king runs at 48 kHz; the chain was built for 44.1 kHz. Nothing about the *geometry* of
    // the block differs, which is exactly why this has to be caught from the header (sec. 4.5).
    Rig rig(host.blockProcessor(), L"engine-format", 48000, 2);
    const std::vector<float> in = signedRamp(kFrames);

    const std::vector<float> passedThrough = rig.run(in);
    CHECK(passedThrough == in);
    REQUIRE(host.chainProcessor().formatMismatches() >= 1);

    const engine::StreamFormat observed = host.chainProcessor().observedFormat();
    CHECK(observed.sampleRate == 48000);
    CHECK(observed.channelCount == 2);
    CHECK(observed.maxFrames == kFrames);

    // The control thread's response: rebuild for what the audio thread saw.
    REQUIRE(host.serviceFormatChange(error));
    INFO("service error: " << error);
    CHECK(host.builtFormat().sampleRate == 48000);

    // The gain set before the rebuild is still set: the rack owns the instance, so re-preparing
    // it for a new sample rate does not construct a new one. This is the behaviour a UI depends
    // on and the reason PluginChain does not own its plugins.
    CHECK(getParameter(host, 0, kGainParam) == Approx(1.0));
    const std::vector<float> processed = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(processed[i] == Approx(in[i] * 2.f));
    }
}

TEST_CASE("the first chain is built from the geometry the audio thread observed",
          "[engine][format]") {
    // What a real attach looks like: the endpoint's format is not known until the king publishes
    // a header (sec. 4.5), so there is nothing to build for until a block has been through.
    constexpr std::int32_t kFrames = 80;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));

    Rig rig(host.blockProcessor(), L"engine-first", 44100, 2);
    const std::vector<float> in = signedRamp(kFrames);

    CHECK_FALSE(host.serviceFormatChange(error)); // no block seen yet
    CHECK(error.empty());

    const std::vector<float> untouched = rig.run(in);
    CHECK(untouched == in);

    REQUIRE(host.serviceFormatChange(error));
    INFO("service error: " << error);
    CHECK(host.builtFormat().sampleRate == 44100);
    CHECK(host.builtFormat().channelCount == 2);
    CHECK(host.builtFormat().maxFrames >= kFrames);

    // Idempotent: a second call with nothing changed must not rebuild.
    rig.run(in);
    CHECK_FALSE(host.serviceFormatChange(error));
    CHECK(error.empty());

    setParameter(host, 0, kGainParam, 1.0);
    const std::vector<float> out = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 2.f));
    }
}

TEST_CASE("a channel count the plugin refuses fails the rebuild, not the stream",
          "[engine][format]") {
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));

    // The fixture refuses anything above 8 channels.
    engine::StreamFormat wide{48000, 12, engine::kDefaultMaxFrames};
    CHECK_FALSE(host.rebuild(wide, error));
    CHECK_FALSE(error.empty());
    CHECK(host.chainProcessor().current() == nullptr);

    // ...and the engine is still usable for a format the plugin does accept.
    REQUIRE(host.rebuild(stereoFormat(), error));
    CHECK(host.chainProcessor().current() != nullptr);
}

TEST_CASE("performEdit from the processing thread is queued, not executed", "[engine][rt]") {
    constexpr std::int32_t kFrames = 64;
    constexpr int kBlocks = 200;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    // Eight performEdit callbacks per block, every one of them from the promoted valet thread.
    setParameter(host, 0, kEditsParam, 1.0);

    Rig rig(host.blockProcessor(), L"engine-edits");
    const std::vector<float> in = signedRamp(kFrames);

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    engine::ComponentHandler* handler = plugin->handler();
    REQUIRE(handler != nullptr);

    std::size_t serviced = 0;
    for (int i = 0; i < kBlocks; ++i) {
        rig.run(in);
        // The ring holds 1023 entries; draining every few blocks is what a control thread would
        // really do, and is what keeps `droppedEdits` at zero.
        if (i % 8 == 0) {
            serviced += host.serviceParameterEdits();
        }
    }
    serviced += host.serviceParameterEdits();

    CHECK(handler->acceptedEdits() > 0);
    CHECK(serviced > 0);
    // The queue is only ever a queue: nothing was dropped, so nothing had to be.
    CHECK(handler->droppedEdits() == 0);
    CHECK(host.droppedParameterEdits() == 0);
}

TEST_CASE("an edit from a plugin editor reaches the processor", "[engine][parameters]") {
    // The path a mouse gesture takes. A plugin's editor talks to its controller and tells the
    // host through IComponentHandler::performEdit; for a split component/controller plugin that
    // is the *only* notification the processor will ever get, so a host that does not carry the
    // value across leaves the audio unchanged and the plugin looking broken.
    //
    // The call below is literally what an editor makes -- our handler, from a thread that is not
    // the audio thread -- and nothing here touches the fixture's own setParamNormalized, which
    // would move the processor's value directly and prove nothing about the host.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    engine::ComponentHandler* handler = plugin->handler();
    REQUIRE(handler != nullptr);

    Rig rig(host.blockProcessor(), L"engine-editor-edit");
    const std::vector<float> in = signedRamp(kFrames);

    // Unity to begin with, so the change is attributable.
    const std::vector<float> before = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(before[i] == Approx(in[i]));
    }

    REQUIRE(handler->performEdit(kGainParam, 1.0) == Steinberg::kResultOk);

    // Queued, not applied: until the control thread services it, nothing has moved.
    const std::vector<float> unserviced = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(unserviced[i] == Approx(in[i]));
    }

    CHECK(host.serviceParameterEdits() == 1);
    CHECK(host.deliveredParameters() == 0); // queued for the audio thread, not yet delivered

    const std::vector<float> after = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(after[i] == Approx(in[i] * 2.f));
    }

    CHECK(host.deliveredParameters() == 1);
    CHECK(host.droppedParameters() == 0);

    // And the edit was not echoed back at the half that made it. A real editor has already moved
    // its controller; pushing the value in again mid-gesture is what makes a knob fight the mouse.
    CHECK(getParameter(host, 0, kGainParam) == Approx(0.5));
}

TEST_CASE("queued values for one parameter collapse to the last", "[engine][parameters]") {
    // Dragging a knob produces a value per mouse event, far more than one per block. They are
    // all stamped at sample offset 0, so the SDK's addPoint replaces rather than appends: the
    // last value wins and the point list never grows. That is what keeps a fast gesture from
    // pushing `inputParameterChanges` past the capacity prepare() warmed for it.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);

    Rig rig(host.blockProcessor(), L"engine-coalesce");
    const std::vector<float> in = signedRamp(kFrames);

    // A sweep ending at 0.25, which the fixture turns into a gain of 0.5. Ending somewhere other
    // than an endpoint matters: a host that delivered the *first* value, or the largest, would
    // land on a different number rather than on a plausible one.
    REQUIRE(plugin->queueParameter(kGainParam, 1.0));
    REQUIRE(plugin->queueParameter(kGainParam, 0.75));
    REQUIRE(plugin->queueParameter(kGainParam, 0.25));

    const std::vector<float> out = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 0.5f));
    }

    CHECK(plugin->deliveredParameters() == 3);
    CHECK(plugin->droppedParameters() == 0);
}

TEST_CASE("a plugin chain in steady state performs exactly zero audio-thread allocations",
          "[engine][rt][soak]") {
    // The sec. 7.4.3 acceptance criterion again, this time with a real VST3 plugin on the call
    // stack rather than an inert processor. The IPC soak in realtime_safety_test.cpp proves our
    // plumbing is clean; this one proves that hosting a plugin did not make it dirty.
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    constexpr std::int32_t kFrames = 128;
    constexpr int kWarmupBlocks = 500;
    constexpr int kSoakBlocks = 5000;
    // Enough to overrun the 1023-slot edit ring at eight edits a block, several times over.
    constexpr int kOverflowBlocks = 2000;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    setParameter(host, 0, kGainParam, 1.0);
    // performEdit on every block too: the lock-free path has to be allocation-free as well.
    setParameter(host, 0, kEditsParam, 1.0);

    Rig rig(host.blockProcessor(), L"engine-soak");
    const std::vector<float> in = signedRamp(kFrames);
    std::vector<float> out(in.size(), 0.f);
    const auto size = static_cast<std::int32_t>(in.size());

    // Warm-up covers the plugin's own first-block laziness as well as our page faults. Sec. 7.4.3
    // allows a transition to be noisy; it is steady state that must be inert.
    for (int i = 0; i < kWarmupBlocks; ++i) {
        REQUIRE(rig.king().dispatch(in.data(), out.data(), size) ==
                harness::DispatchResult::Processed);
        if (i % 64 == 0) {
            host.serviceParameterEdits(4096);
        }
    }
    host.serviceParameterEdits(4096);
    REQUIRE(host.droppedParameterEdits() == 0);

    rt::resetViolations();

    // Serviced regularly, the way a control thread would. 64 blocks is 512 edits, comfortably
    // inside the 1023-slot ring.
    for (int i = 0; i < kSoakBlocks; ++i) {
        if (rig.king().dispatch(in.data(), out.data(), size) !=
            harness::DispatchResult::Processed) {
            FAIL("block " << i << " did not complete the rendezvous");
        }
        if (i % 64 == 0) {
            host.serviceParameterEdits(4096);
        }
    }
    host.serviceParameterEdits(4096);

    const rt::ViolationCounts serviced = rt::violations();
    INFO("blocks processed: " << host.chainProcessor().blocksProcessed());
    INFO("serviced: allocations " << serviced.allocations << " frees " << serviced.deallocations
                                  << " locks " << serviced.locks);

    CHECK(serviced.allocations == 0);
    CHECK(serviced.deallocations == 0);
    CHECK(serviced.locks == 0);
    CHECK(host.droppedParameterEdits() == 0);

    // Now starve the control thread so the ring overflows. Overflow is a policy decision, not a
    // failure -- the audio thread drops the edit and returns -- and the point of the assertion is
    // that it costs exactly as little as the happy path.
    const std::uint64_t droppedBefore = host.droppedParameterEdits();
    for (int i = 0; i < kOverflowBlocks; ++i) {
        if (rig.king().dispatch(in.data(), out.data(), size) !=
            harness::DispatchResult::Processed) {
            FAIL("overflow block " << i << " did not complete the rendezvous");
        }
    }

    const rt::ViolationCounts starved = rt::violations();
    INFO("starved: allocations " << starved.allocations << " frees " << starved.deallocations
                                 << " locks " << starved.locks << " dropped "
                                 << host.droppedParameterEdits());

    CHECK(starved.allocations == 0);
    CHECK(starved.deallocations == 0);
    CHECK(starved.locks == 0);
    CHECK(host.droppedParameterEdits() > droppedBefore);

    CHECK(host.chainProcessor().formatMismatches() == 0);
    CHECK(host.chainProcessor().blocksPassedThrough() == 0);
}

TEST_CASE("delivering parameters into a plugin allocates nothing on the audio thread",
          "[engine][rt][soak]") {
    // Parameter delivery put new work *on* the audio thread -- draining a ring into
    // `inputParameterChanges` at the top of every block -- so it needs the sec. 7.4.3 acceptance
    // criterion applied to it specifically, not just inherited from the chain soak above. The
    // hazard it is guarding is precise: `addParameterData` past the warmed queue count, or
    // `addPoint` past the warmed point count, both allocate inside the SDK.
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    constexpr std::int32_t kFrames = 128;
    constexpr int kWarmupBlocks = 500;
    constexpr int kSoakBlocks = 3000;
    /// More edits per block than the drain bound, so the ring is never empty and the drain hits
    /// its limit every single block rather than only sometimes.
    constexpr int kEditsPerBlock = 24;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    engine::ComponentHandler* handler = plugin->handler();
    REQUIRE(handler != nullptr);

    Rig rig(host.blockProcessor(), L"engine-param-soak");
    const std::vector<float> in = signedRamp(kFrames);
    std::vector<float> out(in.size(), 0.f);
    const auto size = static_cast<std::int32_t>(in.size());

    // A gesture that never stops, across all four of the fixture's parameters so that more than
    // one queue is in play per block.
    const auto gesture = [&](int block) {
        for (int e = 0; e < kEditsPerBlock; ++e) {
            const auto id = static_cast<Steinberg::Vst::ParamID>(e % 4);
            const double value = static_cast<double>((block + e) % 100) / 100.0;
            (void)handler->performEdit(id, value);
        }
        host.serviceParameterEdits(4096);
    };

    for (int i = 0; i < kWarmupBlocks; ++i) {
        gesture(i);
        REQUIRE(rig.king().dispatch(in.data(), out.data(), size) ==
                harness::DispatchResult::Processed);
    }

    rt::resetViolations();

    for (int i = 0; i < kSoakBlocks; ++i) {
        gesture(i);
        if (rig.king().dispatch(in.data(), out.data(), size) !=
            harness::DispatchResult::Processed) {
            FAIL("block " << i << " did not complete the rendezvous");
        }
    }

    const rt::ViolationCounts counts = rt::violations();
    INFO("delivered " << host.deliveredParameters() << " dropped " << host.droppedParameters());
    INFO("allocations " << counts.allocations << " frees " << counts.deallocations << " locks "
                        << counts.locks);

    CHECK(counts.allocations == 0);
    CHECK(counts.deallocations == 0);
    CHECK(counts.locks == 0);
    // The point of the exercise: values really were flowing while all that was zero.
    CHECK(host.deliveredParameters() > static_cast<std::uint64_t>(kSoakBlocks));
}

TEST_CASE("republishing while audio is running retires the old chain safely", "[engine][chain]") {
    constexpr std::int32_t kFrames = 96;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    Rig rig(host.blockProcessor(), L"engine-republish");
    const std::vector<float> in = signedRamp(kFrames);

    // Keep the valet thread busy across each swap. A chain freed while the audio thread was
    // inside it would fault here rather than pass.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 20; ++i) {
            rig.run(in);
        }
        REQUIRE(host.rebuild(stereoFormat(), error));
    }

    setParameter(host, 0, kGainParam, 1.0);
    const std::vector<float> out = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 2.f));
    }

    // Teardown while the thread is live is the other half of the same question.
    host.teardown();
    const std::vector<float> after = rig.run(in);
    CHECK(after == in);
}


// ------------------------------------------------------------------ bus arrangement tiers -----
//
// Three tiers, tried in order, each only when the one before it fails (PluginInstance::prepare):
//
//   1. the arrangement the endpoint's channel mask names
//   2. a guess with the right cardinality
//   3. the plugin's own fixed arrangement, if it is at least as wide as the stream, padded
//
// The tests below take them in that order.

TEST_CASE("a device channel mask names the arrangement a count cannot guess", "[engine][busses]") {
    namespace SpeakerArr = Steinberg::Vst::SpeakerArr;

    // The identity on the bits, which is not a coincidence: VST3 numbered its first eighteen
    // speakers in the SPEAKER_* order. These are the masks Windows actually hands out.
    CHECK(engine::speakerArrangementForMask(0x00000003u, 2) == SpeakerArr::kStereo);
    CHECK(engine::speakerArrangementForMask(0x0000003fu, 6) == SpeakerArr::k51);
    CHECK(engine::speakerArrangementForMask(0x00000033u, 4) == SpeakerArr::k40Music);

    // The one that matters. KSAUDIO_SPEAKER_7POINT1_SURROUND puts the extra pair at the *sides*;
    // the count-based guess reaches for k71Cine, which puts them at front-of-centre. Same eight
    // channels, different speakers -- and nothing but the mask can tell them apart.
    CHECK(engine::speakerArrangementForMask(0x0000063fu, 8) == SpeakerArr::k71Music);
    CHECK(engine::speakerArrangementFor(8) == SpeakerArr::k71Cine);
    CHECK(engine::speakerArrangementForMask(0x0000063fu, 8) != engine::speakerArrangementFor(8));

    // Windows spells mono SPEAKER_FRONT_CENTER, whose bit is VST3's *centre* channel. Passing
    // that through would offer a mono plugin a one-channel surround layout.
    CHECK(engine::speakerArrangementForMask(0x00000004u, 1) == SpeakerArr::kMono);

    SECTION("a mask that does not describe this stream is discarded, not trusted") {
        // The device is configured for 5.1 and the block header says stereo. The header wins:
        // it is read every block, and the mask is a property of a device the stream may have
        // stopped matching.
        CHECK(engine::speakerArrangementForMask(0x0000003fu, 2) == SpeakerArr::kEmpty);
        // No mask at all -- a plain WAVEFORMATEX, which is legal and common.
        CHECK(engine::speakerArrangementForMask(0u, 2) == SpeakerArr::kEmpty);
        // SPEAKER_ALL, and anything else above the eighteen speakers Windows names.
        CHECK(engine::speakerArrangementForMask(0x80000003u, 2) == SpeakerArr::kEmpty);
    }
}

TEST_CASE("a mask that agrees with the guess still gets negotiated", "[engine][busses]") {
    // Tier 1 and tier 2 name the same arrangement for every stereo endpoint in the world, which
    // makes "deduplicate the two candidates" the easiest thing in this file to get wrong: drop
    // both as duplicates of each other and nothing is ever asked for, leaving the plugin on
    // whatever arrangement it was created with. The fixture happens to default to stereo, so the
    // audio would come out right and only a plugin defaulting to something else would ever
    // notice -- by being refused for a width it would have accepted.
    //
    // The fixture declares mono busses and accepts up to eight channels, so a prepare that
    // skipped both candidates would fall to tier 3, read mono back, and refuse the stereo stream
    // outright. Getting two channels here is the evidence that stereo was actually asked for.
    //
    // KSAUDIO_SPEAKER_STEREO, which is what an ordinary pair of speakers reports.
    engine::Engine host;
    host.setChannelMask(0x00000003u);

    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->inputChannelCount() == 2);
    CHECK(plugin->outputChannelCount() == 2);
    CHECK_FALSE(plugin->padded());

    CHECK(host.channelMask() == 0x00000003u);
}

TEST_CASE("a plugin with one fixed wide bus is padded, not refused", "[engine][busses]") {
    // The case this whole path exists for: a plugin built around one fixed eight-channel bus,
    // asked to run on a stereo stream. Tiers 1 and 2 both get refused, and tier 3 meets it at its
    // own width rather than telling the user it cannot be loaded.
    constexpr std::int32_t kFrames = 32;

    engine::Engine host;
    std::string error;
    REQUIRE(host.insertPluginByClassId(0, kTestPluginPath, widePluginClassId(), error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->prepared());
    CHECK(plugin->padded());
    CHECK(plugin->inputChannelCount() == 8);
    CHECK(plugin->outputChannelCount() == 8);
    // The stream is still stereo. Padding is what the plugin sees, never what the king does.
    CHECK(plugin->format().channelCount == 2);

    Rig rig(host.blockProcessor(), L"engine-wide-one");

    // Not signedRamp: its two channels are negatives of each other, so the plugin's bus sum would
    // be zero and the padding could hold anything at all without changing a number.
    std::vector<float> in(static_cast<std::size_t>(kFrames) * 2);
    for (std::int32_t f = 0; f < kFrames; ++f) {
        in[static_cast<std::size_t>(f) * 2] = static_cast<float>(f + 1);
        in[static_cast<std::size_t>(f) * 2 + 1] = 2.f * static_cast<float>(f + 1);
    }

    const std::vector<float> out = rig.run(in);

    // Each output channel is its own input plus the sum of the whole eight-channel bus. With the
    // six padding channels genuinely silent that sum is a + b, so:
    //
    //     out0 = a + (a + b) = 2a + b    with a = k, b = 2k  ->  4k
    //     out1 = b + (a + b) = a + 2b                        ->  5k
    //
    // Any padding channel carrying something other than zero raises both numbers.
    for (std::int32_t f = 0; f < kFrames; ++f) {
        const float k = static_cast<float>(f + 1);
        REQUIRE(out[static_cast<std::size_t>(f) * 2] == Approx(4.f * k));
        REQUIRE(out[static_cast<std::size_t>(f) * 2 + 1] == Approx(5.f * k));
    }
}

TEST_CASE("the padding is silence again for every plugin, not just the first",
          "[engine][busses]") {
    // The bug this is here to catch: fill the padding once at the top of the block and it stops
    // being silence the moment the first plugin writes its own full output width into the bank.
    // The second plugin then reads whatever the first one made of it.
    //
    // The fixture makes that visible rather than subtle. Plugin one writes a + b into all six of
    // its padding outputs, so a host that does not re-zero hands plugin two six copies of it.
    constexpr std::int32_t kFrames = 16;

    engine::Engine host;
    std::string error;
    const std::string wide = widePluginClassId();
    REQUIRE(host.insertPluginByClassId(0, kTestPluginPath, wide, error));
    REQUIRE(host.insertPluginByClassId(1, kTestPluginPath, wide, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    Rig rig(host.blockProcessor(), L"engine-wide-two");

    std::vector<float> in(static_cast<std::size_t>(kFrames) * 2);
    for (std::int32_t f = 0; f < kFrames; ++f) {
        in[static_cast<std::size_t>(f) * 2] = static_cast<float>(f + 1);
        in[static_cast<std::size_t>(f) * 2 + 1] = 2.f * static_cast<float>(f + 1);
    }

    const std::vector<float> out = rig.run(in);

    // With a = k and b = 2k, plugin one leaves (4k, 5k) on the stream channels and 3k on each of
    // its six padding outputs. Re-zeroed, plugin two sees a bus sum of 9k:
    //
    //     out0 = 4k + 9k = 13k
    //     out1 = 5k + 9k = 14k
    //
    // Left alone, its bus sum would be 9k + 6 * 3k = 27k and the first channel would read 31k.
    for (std::int32_t f = 0; f < kFrames; ++f) {
        const float k = static_cast<float>(f + 1);
        REQUIRE(out[static_cast<std::size_t>(f) * 2] == Approx(13.f * k));
        REQUIRE(out[static_cast<std::size_t>(f) * 2 + 1] == Approx(14.f * k));
    }
}

TEST_CASE("a plugin narrower than the stream is still refused", "[engine][busses]") {
    // Padding goes one way only. A plugin wider than the stream is fed silence it can ignore; a
    // plugin narrower than the stream would have to drop channels, and there is no honest way to
    // do that -- so this stays a refusal, reported before anything is published.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));

    // TestPlugin refuses any arrangement above eight channels and sits at stereo when it does.
    const engine::StreamFormat wideStream{48000, 16, engine::kDefaultMaxFrames};
    CHECK_FALSE(host.rebuild(wideStream, error));
    CHECK_FALSE(error.empty());
    INFO("refusal: " << error);
    CHECK(error.find("accepts at most") != std::string::npos);

    // Nothing published, and the rack survives to be rebuilt at a width the plugin will take.
    CHECK(host.chainProcessor().current() == nullptr);
    REQUIRE(host.rebuild(stereoFormat(), error));
    CHECK(host.chainProcessor().current() != nullptr);
}
