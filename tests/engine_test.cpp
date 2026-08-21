// The VST3 host, driven end to end (design_doc.md sec. 7.4.3, sec. 7.4.5).
//
// Every test here runs a real VST3 module -- `tests/fixtures/aip_test_plugin` -- through the real
// valet thread, fed by the synthetic king. Nothing is mocked below the protocol, because the
// interesting failures (buffers wired to the wrong channel, a plugin allocating on the audio
// thread, a chain freed while it is being read) only appear when all three are running together.
//
// The fixture's contract, restated so the expected numbers below can be read without opening it:
//
//   Gain  (id 0)  output = input * (normalized * 2); the 0.5 default is unity
//   Edits (id 1)  performEdit calls per process(), from the audio thread = round(normalized * 8)
//   it refuses any bus arrangement above 8 channels

#include "harness/synthetic_king.h"
#include "harness/wait_for.h"

#include "aip/engine/engine.h"
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

void setParameter(engine::Engine& host, std::size_t pluginIndex, Steinberg::Vst::ParamID id,
                  double normalized) {
    engine::PluginChain* chain = host.chainProcessor().current();
    REQUIRE(chain != nullptr);
    REQUIRE(chain->size() > pluginIndex);
    Steinberg::Vst::IEditController* controller = chain->at(pluginIndex).controller();
    REQUIRE(controller != nullptr);
    REQUIRE(controller->setParamNormalized(id, normalized) == Steinberg::kResultOk);
}

} // namespace

TEST_CASE("the test plugin module loads and exposes one audio effect", "[engine][module]") {
    std::string error;
    engine::PluginModule::Ptr module = engine::PluginModule::load(kTestPluginPath, error);

    INFO("path: " << kTestPluginPath << " error: " << error);
    REQUIRE(module != nullptr);
    CHECK(error.empty());
    REQUIRE(module->audioEffects().size() == 1);
    CHECK(module->audioEffects().front().name == "AIP Test Plugin");
    CHECK(module->audioEffects().front().vendor == "audio-ipc2");
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

    // The rebuilt chain is a fresh instance, so the gain is back at its default.
    setParameter(host, 0, kGainParam, 1.0);
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

    engine::PluginChain* chain = host.chainProcessor().current();
    REQUIRE(chain != nullptr);
    engine::ComponentHandler* handler = chain->at(0).handler();
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
