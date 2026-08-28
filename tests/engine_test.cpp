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
//   Latency (id 4)  reported latency = round(normalized * 512), announced with kLatencyChanged
//                   and withheld from getLatencySamples until the next setActive(true)
//   Restart (id 5)  restartComponent(round(normalized * 2047)) -- the raw flag word
//   Echo    (id 6)  above 0.5, every setActive(true) announces kLatencyChanged again
//   it refuses any bus arrangement above 8 channels, and carries a default-active side-chain

#include "harness/synthetic_king.h"
#include "harness/wait_for.h"

#include "aip/engine/engine.h"
#include "aip/engine/plugin_instance.h"
#include "aip/ipc/buffer_valet.h"
#include "aip/ipc/valet_thread.h"
#include "aip/rt/realtime_guard.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"

#include "pluginterfaces/gui/iplugview.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

using namespace aip;
using Catch::Approx;

namespace {

/// Written by CMake; there is no way for the test to work the bundle path out at run time.
const std::string kTestPluginPath = AIP_TEST_PLUGIN_PATH;

constexpr Steinberg::Vst::ParamID kGainParam = 0;
constexpr Steinberg::Vst::ParamID kEditsParam = 1;
constexpr Steinberg::Vst::ParamID kOffsetParam = 3;
constexpr Steinberg::Vst::ParamID kLatencyParam = 4;
constexpr Steinberg::Vst::ParamID kRestartParam = 5;
constexpr Steinberg::Vst::ParamID kEchoParam = 6;

/// The fixture's `Latency` at 1.0, in samples. Halve the parameter, halve this.
constexpr std::uint32_t kFixtureMaxLatency = 512;

/// The normalized value that makes the fixture raise exactly `flags`. It maps [0, 1] onto
/// [0, kRestartFlagSpan] and rounds, so this is that mapping read backwards -- and the span has to
/// agree with the fixture's, which is why both spell it out rather than sharing a header no other
/// test would want.
constexpr Steinberg::Vst::ParamValue restartRequest(std::int32_t flags) {
    return static_cast<Steinberg::Vst::ParamValue>(flags) / 2047.0;
}

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
void setParameter(engine::Engine& host, std::size_t pluginIndex, Steinberg::Vst::ParamID id, double normalized) {
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

/// The class id of one of the module's plugins by name, resolved rather than hardcoded: a UID
/// written out by hand is a test that fails for the wrong reason the day the fixture is edited.
std::string classIdNamed(std::string_view name) {
    std::string error;
    engine::PluginModule::Ptr module = engine::PluginModule::load(kTestPluginPath, error);
    REQUIRE(module != nullptr);
    for (const engine::PluginClass& info : module->audioEffects()) {
        if (info.name == name) {
            return info.id.toString();
        }
    }
    FAIL("the test plugin module has no class named " << name);
    return {};
}

std::string widePluginClassId() { return classIdNamed("AIP Wide Plugin"); }

std::string namelessBusPluginClassId() { return classIdNamed("AIP Nameless Bus Plugin"); }

} // namespace

TEST_CASE("the test plugin module loads and exposes one audio effect", "[engine][module]") {
    std::string error;
    engine::PluginModule::Ptr module = engine::PluginModule::load(kTestPluginPath, error);

    INFO("path: " << kTestPluginPath << " error: " << error);
    REQUIRE(module != nullptr);
    CHECK(error.empty());
    REQUIRE(module->audioEffects().size() == 3);
    CHECK(module->audioEffects().front().name == "AIP Test Plugin");
    CHECK(module->audioEffects().front().vendor == "audio-ipc2");
    // Not first, so that `appendPlugin` -- which takes the first audio effect -- goes on meaning
    // TestPlugin everywhere below.
    CHECK(module->audioEffects()[1].name == "AIP Wide Plugin");
    CHECK(module->audioEffects()[2].name == "AIP Nameless Bus Plugin");
}

TEST_CASE("loading something that is not a plugin fails without throwing", "[engine][module]") {
    std::string error;
    engine::PluginModule::Ptr module = engine::PluginModule::load("D:/definitely/not/here.vst3", error);

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
    REQUIRE(component->getBusInfo(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 1, aux) == Steinberg::kResultOk);
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

TEST_CASE("the chain bypass hands the block back untouched", "[engine][chain]") {
    // The whole-chain bypass, which is a different thing from bypassing every plugin in the rack:
    // the chain stays published and prepared, and the audio thread simply never reaches it. What
    // this proves is that it is bit-exact -- a bypass that ran anything at all would show up here
    // as a scaled ramp -- and that nothing about the rack was disturbed on the way through.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    setParameter(host, 0, kGainParam, 1.0); // 2x
    setParameter(host, 1, kGainParam, 1.0); // 2x again: 4x through the chain

    CHECK_FALSE(host.chainBypassed());

    Rig rig(host.blockProcessor(), L"engine-chain-bypass");
    const std::vector<float> in = signedRamp(kFrames);

    const std::vector<float> processed = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(processed[i] == Approx(in[i] * 4.f));
    }

    const std::uint64_t ranBefore = host.chainProcessor().blocksProcessed();
    const std::uint64_t passedBefore = host.chainProcessor().blocksPassedThrough();

    host.setChainBypass(true);
    CHECK(host.chainBypassed());

    // Bit-for-bit, not approximately: the samples the king published are the samples it gets
    // back, because nothing touched them.
    const std::vector<float> bypassed = rig.run(in);
    CHECK(bypassed == in);
    CHECK(host.chainProcessor().blocksBypassed() == 1);
    // A bypassed block is passed through, and says why. Neither counter may claim it was run.
    CHECK(host.chainProcessor().blocksPassedThrough() == passedBefore + 1);
    CHECK(host.chainProcessor().blocksProcessed() == ranBefore);

    // Nothing left the rack and nothing was unpublished -- that is what makes coming back
    // instantaneous, and what keeps every parameter the user set.
    CHECK(host.pluginCount() == 2);
    REQUIRE(host.chainProcessor().current() != nullptr);
    CHECK(host.chainProcessor().current()->size() == 2);

    host.setChainBypass(false);
    const std::vector<float> restored = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(restored[i] == Approx(in[i] * 4.f));
    }
}

TEST_CASE("the chain bypass survives what happens to the rack under it", "[engine][chain]") {
    // It is a property of the chain, not of a chain object: publishing a new view over the rack
    // must not quietly switch the audio back on. Every one of these republishes.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    host.setChainBypass(true);

    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    CHECK(host.chainBypassed());

    REQUIRE(host.setBypass(0, true));
    CHECK(host.chainBypassed());

    REQUIRE(host.rebuild(stereoFormat(44100), error));
    CHECK(host.chainBypassed());

    host.teardown();
    CHECK(host.chainBypassed());

    // And with nothing in the rack at all, which is a chain someone can perfectly well have
    // switched out of the path before they built it.
    host.clearPlugins();
    CHECK(host.chainBypassed());
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

    setParameter(host, 0, kGainParam, 1.0); // 2x, no offset
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

TEST_CASE("a format the chain was not built for is passed through and reported", "[engine][format]") {
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

TEST_CASE("the first chain is built from the geometry the audio thread observed", "[engine][format]") {
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

TEST_CASE("a channel count the plugin refuses fails the rebuild, not the stream", "[engine][format]") {
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

TEST_CASE("a plugin chain in steady state performs exactly zero audio-thread allocations", "[engine][rt][soak]") {
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
        REQUIRE(rig.king().dispatch(in.data(), out.data(), size) == harness::DispatchResult::Processed);
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
        if (rig.king().dispatch(in.data(), out.data(), size) != harness::DispatchResult::Processed) {
            FAIL("block " << i << " did not complete the rendezvous");
        }
        if (i % 64 == 0) {
            host.serviceParameterEdits(4096);
        }
    }
    host.serviceParameterEdits(4096);

    const rt::ViolationCounts serviced = rt::violations();
    INFO("blocks processed: " << host.chainProcessor().blocksProcessed());
    INFO("serviced: allocations " << serviced.allocations << " frees " << serviced.deallocations << " locks "
                                  << serviced.locks);

    CHECK(serviced.allocations == 0);
    CHECK(serviced.deallocations == 0);
    CHECK(serviced.locks == 0);
    CHECK(host.droppedParameterEdits() == 0);

    // Now starve the control thread so the ring overflows. Overflow is a policy decision, not a
    // failure -- the audio thread drops the edit and returns -- and the point of the assertion is
    // that it costs exactly as little as the happy path.
    const std::uint64_t droppedBefore = host.droppedParameterEdits();
    for (int i = 0; i < kOverflowBlocks; ++i) {
        if (rig.king().dispatch(in.data(), out.data(), size) != harness::DispatchResult::Processed) {
            FAIL("overflow block " << i << " did not complete the rendezvous");
        }
    }

    const rt::ViolationCounts starved = rt::violations();
    INFO("starved: allocations " << starved.allocations << " frees " << starved.deallocations << " locks "
                                 << starved.locks << " dropped " << host.droppedParameterEdits());

    CHECK(starved.allocations == 0);
    CHECK(starved.deallocations == 0);
    CHECK(starved.locks == 0);
    CHECK(host.droppedParameterEdits() > droppedBefore);

    CHECK(host.chainProcessor().formatMismatches() == 0);
    CHECK(host.chainProcessor().blocksPassedThrough() == 0);
}

TEST_CASE("delivering parameters into a plugin allocates nothing on the audio thread", "[engine][rt][soak]") {
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
        REQUIRE(rig.king().dispatch(in.data(), out.data(), size) == harness::DispatchResult::Processed);
    }

    rt::resetViolations();

    for (int i = 0; i < kSoakBlocks; ++i) {
        gesture(i);
        if (rig.king().dispatch(in.data(), out.data(), size) != harness::DispatchResult::Processed) {
            FAIL("block " << i << " did not complete the rendezvous");
        }
    }

    const rt::ViolationCounts counts = rt::violations();
    INFO("delivered " << host.deliveredParameters() << " dropped " << host.droppedParameters());
    INFO("allocations " << counts.allocations << " frees " << counts.deallocations << " locks " << counts.locks);

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

TEST_CASE("a plugin that will not name its wide bus is padded, not refused", "[engine][busses]") {
    // The bug the fallback in tier 3 exists for. `AIP Nameless Bus Plugin` refuses every
    // arrangement but its own stereo-in / fifteen-out, and then declines `getBusArrangement` on
    // the output -- which is not obstinacy but the only honest answer a JUCE plugin with an
    // `AudioChannelSet::discreteChannels(15)` bus can give, since no set of VST3 speaker bits
    // means "fifteen channels with no assigned positions". `getBusInfo` still says fifteen.
    //
    // Reading the unanswered call as a refusal is what used to happen, and it reported the
    // plugin as unloadable ("would not say which bus arrangement it wants") over channel roles
    // this protocol never carries (sec. 4.3). soundscape_zone_demo_cu was the specimen.
    constexpr std::int32_t kFrames = 32;
    constexpr std::uint32_t kNamelessChannels = 15;

    engine::Engine host;
    std::string error;
    REQUIRE(host.insertPluginByClassId(0, kTestPluginPath, namelessBusPluginClassId(), error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->prepared());
    CHECK(plugin->padded());
    // Asymmetric on purpose, and the shape of the specimen: the input bus names itself and takes
    // the stream width, the output bus does not and keeps its own.
    CHECK(plugin->inputChannelCount() == 2);
    CHECK(plugin->outputChannelCount() == kNamelessChannels);
    CHECK(plugin->format().channelCount == 2);

    Rig rig(host.blockProcessor(), L"engine-nameless-bus");

    std::vector<float> in(static_cast<std::size_t>(kFrames) * 2);
    for (std::int32_t f = 0; f < kFrames; ++f) {
        in[static_cast<std::size_t>(f) * 2] = static_cast<float>(f + 1);
        in[static_cast<std::size_t>(f) * 2 + 1] = 2.f * static_cast<float>(f + 1);
    }

    const std::vector<float> out = rig.run(in);

    // The wide plugin's arithmetic, for the wide plugin's reason: every output channel is its own
    // input plus the sum of the whole input bus, so out0 = 2a + b and out1 = a + 2b with a = k
    // and b = 2k. The input bus is unpadded here, so what these numbers actually pin down is that
    // the host read the *negotiated* widths off the buffers rather than assuming the two busses
    // agreed -- get that wrong and the thirteen surplus output channels are written somewhere.
    for (std::int32_t f = 0; f < kFrames; ++f) {
        const float k = static_cast<float>(f + 1);
        REQUIRE(out[static_cast<std::size_t>(f) * 2] == Approx(4.f * k));
        REQUIRE(out[static_cast<std::size_t>(f) * 2 + 1] == Approx(5.f * k));
    }
}

TEST_CASE("the padding is silence again for every plugin, not just the first", "[engine][busses]") {
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

// -------------------------------------------------------------- the host's own parameter path -

TEST_CASE("a plugin is allowed to have no editor of its own", "[engine][editor]") {
    // The premise of the generic editor, pinned here because it is a fact about VST3 rather than
    // about our code: `createView(kEditor)` returning null is a legal answer, not a failure. The
    // fixture gives it -- it never overrides createView -- which is what makes it the specimen
    // the shell's fallback editor is built against.
    std::string error;
    engine::PluginModule::Ptr module = engine::PluginModule::load(kTestPluginPath, error);
    REQUIRE(module != nullptr);

    Steinberg::IPtr<Steinberg::Vst::HostApplication> hostContext =
        Steinberg::owned(new Steinberg::Vst::HostApplication());
    std::unique_ptr<engine::PluginInstance> instance =
        engine::PluginInstance::create(module, module->audioEffects().front().id, hostContext, error);
    REQUIRE(instance != nullptr);

    Steinberg::Vst::IEditController* controller = instance->controller();
    REQUIRE(controller != nullptr);

    Steinberg::IPtr<Steinberg::IPlugView> view =
        Steinberg::owned(controller->createView(Steinberg::Vst::ViewType::kEditor));
    CHECK(view == nullptr);

    // ...and it still has parameters, which is the whole reason a fallback editor is worth
    // building rather than reporting "no editor" and stopping.
    CHECK(controller->getParameterCount() > 0);
}

TEST_CASE("a host-originated parameter edit reaches both halves of the plugin", "[engine][params]") {
    // A plugin's own editor sets its controller itself and reports the edit through
    // IComponentHandler, and Engine::serviceParameterEdits carries that across to the processor.
    // An edit made in a window of *ours* has neither going for it, so `setParameter` has to do
    // both -- and this checks both, separately, because the fixture is a single component and
    // would sound right from the controller call alone.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    const std::uint64_t deliveredBefore = plugin->deliveredParameters();

    CHECK(plugin->setParameter(kGainParam, 1.0));

    // The controller half: what the shell's own slider and the plugin's value string read from.
    CHECK(getParameter(host, 0, kGainParam) == Approx(1.0));

    Rig rig(host.blockProcessor(), L"engine-host-param");
    const std::vector<float> in = signedRamp(kFrames);
    const std::vector<float> out = rig.run(in);

    // The processor half, twice over: the audio changed, and it changed because a value came
    // through `inputParameterChanges` rather than because a single-component plugin happens to
    // be its own controller.
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 2.f));
    }
    CHECK(plugin->deliveredParameters() > deliveredBefore);
}

TEST_CASE("a host-originated edit survives a rebuild, like any other", "[engine][params]") {
    // The rack outlives the chain, so a value set through the shell has to come back after a
    // format change the same way one set in a plugin's own editor does. Cheap to check and the
    // kind of thing that quietly stops being true.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->setParameter(kOffsetParam, 0.25));

    REQUIRE(host.rebuild(stereoFormat(96000), error));
    INFO("rebuild error: " << error);

    // Same instance, because re-preparing happens in place.
    CHECK(host.pluginAt(0) == plugin);
    CHECK(getParameter(host, 0, kOffsetParam) == Approx(0.25));
}

// ------------------------------------------------------------------------------- warm-up ------

TEST_CASE("a plugin is exercised the moment it is prepared, not the first time audio arrives", "[engine][warmup]") {
    // Left alone, a plugin's first-call behaviour -- allocating, building tables, faulting --
    // happens on the valet thread the first time the user plays something, which may be hours
    // after they loaded it and with nothing on screen connecting the two. The warm-up moves it
    // to the moment they pressed the button.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    INFO("rebuild error: " << error);

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);

    // No audio has been dispatched at all at this point -- there is not even a valet attached.
    CHECK(plugin->processCalls() == engine::Engine::kWarmUpBlocks);

    const engine::Engine::WarmUpReport& report = host.lastWarmUp();
    CHECK(report.ran());
    CHECK(report.plugins == 1);
    CHECK(report.blocks == engine::Engine::kWarmUpBlocks);
    CHECK(report.blocksFailed == 0);
}

TEST_CASE("warming up costs the process no counted violations", "[engine][warmup][rt]") {
    // Two claims, and the second one is a limitation rather than a feature -- pinned here because
    // it is exactly the sort of thing someone reads the warm-up's report and assumes away.
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    rt::resetViolations();

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    const engine::Engine::WarmUpReport& report = host.lastWarmUp();
    REQUIRE(report.ran());

    // One: nothing the warm-up did was charged to the process. That is the whole point of running
    // it under a probe -- a warm-up that moved these counters would make sec. 7.4.3's "exactly
    // zero" mean "zero plus whatever we did on purpose", which is not a criterion at all.
    CHECK(rt::violations().total() == 0);

    // Two: the probe saw nothing either, *and this is expected*. The fixture allocates on its
    // first process call on purpose (see aip_test_plugin.cpp), and the detector still reports
    // zero -- because it replaces `operator new` per image and a VST3 plugin is a DLL carrying
    // its own, as rt/src/alloc_hooks.cpp says in as many words.
    //
    // So the warm-up cannot tell anyone what a plugin allocates. It can only make the plugin do
    // it here rather than on the valet thread, which needs no counter. Anything nonzero in this
    // field is our own processing path misbehaving, and would be a defect.
    INFO("allocations " << report.violations.allocations << " frees " << report.violations.deallocations << " locks "
                        << report.violations.locks);
    CHECK(report.violations.total() == 0);

    rt::resetViolations();
}

TEST_CASE("a plugin inserted into a running rack is warmed before it can be reached", "[engine][warmup]") {
    // The other prepare site. Here there is no retract to lean on -- the instance is warmed while
    // it is still outside the rack, which is what makes it unreachable from the audio thread.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    Rig rig(host.blockProcessor(), L"engine-warmup-insert");
    const std::vector<float> in = signedRamp(32);
    (void)rig.run(in);

    REQUIRE(host.insertPlugin(1, kTestPluginPath, error));
    INFO("insert error: " << error);

    engine::PluginInstance* inserted = host.pluginAt(1);
    REQUIRE(inserted != nullptr);
    CHECK(inserted->processCalls() >= engine::Engine::kWarmUpBlocks);

    const engine::Engine::WarmUpReport& report = host.lastWarmUp();
    CHECK(report.plugins == 1);
    CHECK(report.blocks == engine::Engine::kWarmUpBlocks);

    // The rack still works afterwards, which is the part a warm-up could plausibly break: it
    // leaves parameter queues drained and the plugin's own state advanced by four blocks.
    const std::vector<float> out = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i]));
    }
}

TEST_CASE("warming up does not disturb a restored parameter", "[engine][warmup][params]") {
    // The warm-up calls the same `process` the audio thread does, which drains the parameter ring
    // into the plugin. That is correct -- the values are applied rather than lost -- but it is
    // exactly the kind of side effect worth pinning, because "the session restored but the first
    // four blocks used the wrong gain" is invisible until someone listens closely.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->setParameter(kGainParam, 1.0));

    // A rebuild re-prepares and re-warms with that value already queued.
    REQUIRE(host.rebuild(stereoFormat(96000), error));
    INFO("rebuild error: " << error);
    CHECK(getParameter(host, 0, kGainParam) == Approx(1.0));

    Rig rig(host.blockProcessor(), L"engine-warmup-params", 96000);
    const std::vector<float> in = signedRamp(32);
    const std::vector<float> out = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 2.f));
    }
}

// ------------------------------------------------------------------- speculative preparation --

TEST_CASE("a rack is prepared from the endpoint's format before any block arrives", "[engine][speculative]") {
    // Protocol v1 announces the format nowhere (sec. 4.5), so a client that attaches while
    // nothing is playing used to sit with every plugin unprepared -- no warm-up, and no way to
    // learn that a plugin refuses the format -- until the user happened to play something.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    REQUIRE_FALSE(plugin->prepared());

    REQUIRE(host.prepareSpeculatively(48000, 2, error));
    INFO("guess error: " << error);

    CHECK(plugin->prepared());
    CHECK(host.builtFormat().sampleRate == 48000);
    CHECK(host.builtFormat().channelCount == 2);
    // Prepared *and warmed*, which is the other half of what waiting for a block was costing.
    CHECK(plugin->processCalls() == engine::Engine::kWarmUpBlocks);

    // Flagged, because "prepared" on a guess is a weaker claim than "prepared" on a block.
    CHECK(host.builtFormatIsSpeculative());
}

TEST_CASE("a correct guess is confirmed by the first block rather than rebuilt", "[engine][speculative]") {
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.prepareSpeculatively(48000, 2, error));
    REQUIRE(host.builtFormatIsSpeculative());

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    const std::uint64_t warmedCalls = plugin->processCalls();

    Rig rig(host.blockProcessor(), L"engine-guess-right", 48000, 2);
    const std::vector<float> in = signedRamp(64);
    const std::vector<float> out = rig.run(in);

    // Processed from the very first block: no pass-through, because the chain was already there.
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i]));
    }
    CHECK(host.chainProcessor().blocksProcessed() >= 1);
    CHECK(host.chainProcessor().blocksPassedThrough() == 0);

    // Servicing now confirms the guess without rebuilding -- same instance, no second warm-up.
    REQUIRE_FALSE(host.serviceFormatChange(error));
    CHECK(error.empty());
    CHECK_FALSE(host.builtFormatIsSpeculative());
    CHECK(host.pluginAt(0) == plugin);
    CHECK(plugin->processCalls() == warmedCalls + 1);
}

TEST_CASE("a wrong guess costs one passed-through block and then corrects itself", "[engine][speculative]") {
    // The safety argument for guessing at all. ChainProcessor compares every block against the
    // format the chain was built for and passes it through untouched on a mismatch, so a wrong
    // guess is a handled case rather than a hazard.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));

    // The endpoint claims 44100; the king will actually produce 48000.
    REQUIRE(host.prepareSpeculatively(44100, 2, error));
    REQUIRE(host.builtFormat().sampleRate == 44100);

    Rig rig(host.blockProcessor(), L"engine-guess-wrong", 48000, 2);
    const std::vector<float> in = signedRamp(64);

    setParameter(host, 0, kGainParam, 1.0);
    const std::vector<float> first = rig.run(in);

    // Untouched, because the chain was built for a rate this block is not.
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(first[i] == Approx(in[i]));
    }
    CHECK(host.chainProcessor().formatMismatches() >= 1);

    // The control thread now sees what was really observed and rebuilds for it.
    REQUIRE(host.serviceFormatChange(error));
    INFO("rebuild error: " << error);
    CHECK(host.builtFormat().sampleRate == 48000);
    CHECK_FALSE(host.builtFormatIsSpeculative());

    // And the gain set before the correction survived it, because the rack outlives the chain.
    const std::vector<float> second = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(second[i] == Approx(in[i] * 2.f));
    }
}

TEST_CASE("guessing does not overwrite a format a block already established", "[engine][speculative]") {
    // Re-attaching to the same endpoint must not tear down a chain built from real evidence and
    // replace it with one built from a guess.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));
    REQUIRE_FALSE(host.builtFormatIsSpeculative());

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    const std::uint64_t calls = plugin->processCalls();

    REQUIRE(host.prepareSpeculatively(48000, 2, error));

    // Same geometry, so nothing happened at all: no re-prepare, no second warm-up, and the claim
    // is not downgraded from observed to guessed.
    CHECK(plugin->processCalls() == calls);
    CHECK_FALSE(host.builtFormatIsSpeculative());
}

TEST_CASE("an endpoint that reports no format is declined, not guessed at", "[engine][speculative]") {
    // A device that reports a plain WAVEFORMATEX, or none at all, leaves these zero. Inventing a
    // plausible-looking 48 kHz stereo would be asserting something we were not told.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));

    CHECK_FALSE(host.prepareSpeculatively(0, 0, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(host.pluginAt(0)->prepared());

    // Past the ceiling is refused for the same reason a rebuild refuses it.
    CHECK_FALSE(host.prepareSpeculatively(48000, engine::kMaxChannels + 1, error));
    CHECK_FALSE(error.empty());
}

// ------------------------------------------------------------------- restartComponent ---------
//
// The fixture's restart parameters are what these drive: `Latency` (id 4) announces a new latency
// and withholds the figure until reactivation, `Restart` (id 5) raises an arbitrary flag word, and
// `Echo` (id 6) makes the plugin announce a latency change from inside its own reactivation. See
// the top of `aip_test_plugin.cpp`.

TEST_CASE("a plugin that announces a new latency is reactivated, and only then believed", "[engine][restart]") {
    // The whole reason kLatencyChanged is treated as a reconfiguration rather than as a number to
    // re-read: the SDK says `getLatencySamples` is valid only after `setActive(true)`, and the
    // fixture holds itself to that. A host that skipped the rebuild would read 0 here and report
    // it, which is worse than not reporting latency at all.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->latencySamples() == 0);

    // Half of a 512-sample span. The plugin has decided on 256 and said so; it is still reporting
    // 0 to anyone who asks, because it has not been reactivated.
    CHECK(plugin->setParameter(kLatencyParam, 0.5));
    CHECK(plugin->latencySamples() == 0);

    host.serviceParameterEdits();

    const engine::Engine::RestartReport& report = host.lastRestart();
    CHECK(report.any());
    CHECK(report.requests == 1);
    CHECK((report.flags & Steinberg::Vst::kLatencyChanged) != 0);
    CHECK(report.reconfigured);
    CHECK(report.error.empty());
    CHECK(report.suppressed == 0);
    // The same instance -- a re-prepare, not a reload -- carrying the figure it withheld before.
    CHECK(host.pluginAt(0) == plugin);
    CHECK(plugin->latencySamples() == 256);
}

TEST_CASE("a rebuild driven by a restart keeps the rack's parameters and its chain", "[engine][restart]") {
    // A reconfiguration must cost no more than a format change does: the instances survive, their
    // values survive, and audio comes back processed rather than passed through.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->setParameter(kGainParam, 1.0));
    CHECK(plugin->setParameter(kLatencyParam, 1.0));

    host.serviceParameterEdits();
    REQUIRE(host.lastRestart().reconfigured);

    CHECK(getParameter(host, 0, kGainParam) == Approx(1.0));
    CHECK(plugin->latencySamples() == 512);
    CHECK(host.builtFormat().sampleRate == stereoFormat().sampleRate);

    Rig rig(host.blockProcessor(), L"engine-restart-rebuild");
    const std::vector<float> in = signedRamp(kFrames);
    const std::vector<float> out = rig.run(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(out[i] == Approx(in[i] * 2.f));
    }
}

TEST_CASE("a restart that only says the values moved does not touch the chain", "[engine][restart]") {
    // kParamValuesChanged invalidates caches this host does not have. Reporting it and doing
    // nothing is the correct response, and "doing nothing" has to be checkable -- a rebuild would
    // be audible, and audible for no reason at all.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    const std::uint64_t calls = plugin->processCalls();

    CHECK(plugin->setParameter(kRestartParam, restartRequest(Steinberg::Vst::kParamValuesChanged)));
    host.serviceParameterEdits();

    const engine::Engine::RestartReport& report = host.lastRestart();
    CHECK(report.requests == 1);
    CHECK(report.parameterValues);
    CHECK_FALSE(report.reconfigured);
    CHECK(report.unhandled.empty());
    // The discriminator: a rebuild warms every plugin it re-prepares, so one would show up here
    // as four process calls this test did not make.
    CHECK(plugin->processCalls() == calls);
}

TEST_CASE("a flag this host does not act on is named rather than obeyed", "[engine][restart]") {
    // There is no parameter-title cache to invalidate, no MIDI mapping and no routing graph. The
    // useful behaviour is to say which one arrived: silence would leave whoever reads the log
    // unable to tell "ignored" from "never delivered".
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    const std::uint64_t calls = plugin->processCalls();

    CHECK(plugin->setParameter(
        kRestartParam, restartRequest(Steinberg::Vst::kParamTitlesChanged | Steinberg::Vst::kRoutingInfoChanged)));
    host.serviceParameterEdits();

    const engine::Engine::RestartReport& report = host.lastRestart();
    CHECK(report.requests == 1);
    CHECK_FALSE(report.parameterValues);
    CHECK_FALSE(report.reconfigured);
    CHECK(report.unhandled == "parameter titles, routing info");
    CHECK(plugin->processCalls() == calls);
}

TEST_CASE("a restart request with no chain to rebuild is recorded and left alone", "[engine][restart]") {
    // Nothing is prepared, so there is nothing to deactivate. The next prepare *is* the
    // reconfiguration the plugin asked for, and it reads the new latency on its way out of it --
    // the case where doing nothing is not merely safe but correct.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    REQUIRE_FALSE(plugin->prepared());

    CHECK(plugin->setParameter(kLatencyParam, 0.5));
    host.serviceParameterEdits();

    const engine::Engine::RestartReport& report = host.lastRestart();
    CHECK(report.requests == 1);
    CHECK_FALSE(report.reconfigured);
    CHECK(report.error.empty());
    CHECK(plugin->latencySamples() == 0);

    // And the prepare that follows picks the figure up without anybody asking again.
    REQUIRE(host.rebuild(stereoFormat(), error));
    CHECK(plugin->latencySamples() == 256);
}

TEST_CASE("the rebuild does not chase the request its own reactivation raised", "[engine][restart]") {
    // `Echo` is the JUCE-shaped plugin that announces its latency from inside `setActive(true)`.
    // Acting on that announcement produces another one, so a host that acts on every request it
    // sees rebuilds until somebody kills it. One rebuild per servicing tick, and the requests the
    // rebuild itself provoked are counted and dropped.
    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->setParameter(kEchoParam, 1.0));
    CHECK(plugin->setParameter(kLatencyParam, 0.25));

    host.serviceParameterEdits();
    CHECK(host.lastRestart().reconfigured);
    CHECK(host.lastRestart().suppressed == 1);
    CHECK(plugin->latencySamples() == 128);

    // The next tick has nothing left to act on: convergence, which is the property being tested.
    // Without the second drain the echo would still be queued and this tick would rebuild again.
    const std::uint64_t calls = plugin->processCalls();
    host.serviceParameterEdits();
    CHECK_FALSE(host.lastRestart().any());
    CHECK(plugin->processCalls() == calls);
}

TEST_CASE("a restart raised from the processing thread is honoured too", "[engine][restart]") {
    // The two callback paths are separate by construction: the audio thread pushes onto a
    // lock-free ring and every other thread appends to a vector under a lock. A host that drained
    // only the second would pass every test above and drop this one -- and this is the path a real
    // plugin takes when it is the *processor* that decides its latency has changed.
    constexpr std::int32_t kFrames = 64;

    engine::Engine host;
    std::string error;
    REQUIRE(host.appendPlugin(kTestPluginPath, error));
    REQUIRE(host.rebuild(stereoFormat(), error));

    engine::PluginInstance* plugin = host.pluginAt(0);
    REQUIRE(plugin != nullptr);

    Rig rig(host.blockProcessor(), L"engine-restart-audio");
    // Queued for the processor rather than set on the controller, so the fixture reads it out of
    // `inputParameterChanges` inside `process` and calls back from the valet thread.
    REQUIRE(plugin->queueParameter(kLatencyParam, 0.5));
    (void)rig.run(signedRamp(kFrames));

    host.serviceParameterEdits();

    const engine::Engine::RestartReport& report = host.lastRestart();
    CHECK(report.any());
    CHECK((report.flags & Steinberg::Vst::kLatencyChanged) != 0);
    CHECK(report.reconfigured);
    CHECK(plugin->latencySamples() == 256);
}
