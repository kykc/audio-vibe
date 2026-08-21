// Protocol v1 conformance harness (design_doc.md sec. 4.7).
//
// The synthetic king drives blocks from the test thread; the real client valet runs on its own
// promoted thread, exactly as it does in production. Coverage follows the sec. 4.7 list: attach and
// detach, takeover (`Stolen`), king-side timeout eviction, format change mid-stream, planar
// round-trip fidelity, and the stale-`sampleRate` case of sec. 4.5.
//
// This replaces the predecessor's manual interactive `DebugStream`: the client is developed and
// regression-tested without `audiodg.exe` anywhere in the loop.

#include "harness/synthetic_king.h"
#include "harness/test_processors.h"
#include "harness/wait_for.h"

#include "aip/ipc/buffer_valet.h"
#include "aip/ipc/valet_supervisor.h"
#include "aip/ipc/valet_thread.h"

#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <vector>

using namespace aip;
using harness::waitFor;

namespace {

/// A block of interleaved audio plus somewhere to collect the king's output.
struct TestBlock {
    TestBlock(std::uint32_t channelCount, std::int32_t frames)
        : size(frames * static_cast<std::int32_t>(channelCount)),
          input(static_cast<std::size_t>(size)), output(static_cast<std::size_t>(size), 0.f) {
        // A ramp, so that a channel swap or an off-by-one cannot pass unnoticed.
        std::iota(input.begin(), input.end(), 1.f);
    }

    std::int32_t size;
    std::vector<float> input;
    std::vector<float> output;
};

/// A short king-side timeout. Sec. 4.4 specifies 1000 ms, and `SyntheticKing` defaults to
/// that, but tests that deliberately provoke eviction would otherwise spend a full second
/// per block.
constexpr DWORD kFastEvictionMs = 60;

} // namespace

TEST_CASE("valet attaches, claims the stream, and detaches cleanly", "[conformance][attach]") {
    const std::wstring base = harness::uniqueTestObjectBase(L"attach");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));
    REQUIRE(king.valetIdInHeader() == protocol::kNoValet);

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base)); // sec. 4.4 attach steps 1-5

    // Step 3: the id is random and nonzero; step 5: it is published at offset 0.
    REQUIRE(valet.valetId() != protocol::kNoValet);
    REQUIRE(king.valetIdInHeader() == valet.valetId());
    REQUIRE(valet.attached());

    valet.detach();

    // A clean detach releases the claim, so the king reverts to passing audio through (sec. 4.4).
    REQUIRE(king.valetIdInHeader() == protocol::kNoValet);
    REQUIRE_FALSE(valet.attached());

    TestBlock block(2, 16);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::NoValet);
    REQUIRE(block.output == block.input);
}

TEST_CASE("attach fails when the endpoint is not active", "[conformance][attach]") {
    // Sec. 4.4 step 1: if either event does not exist there is no stream -- fail and retry later.
    ipc::BufferValet valet;
    REQUIRE_FALSE(valet.attach(harness::uniqueTestObjectBase(L"absent")));
    REQUIRE_FALSE(valet.attached());
}

TEST_CASE("a block round-trips through the valet unchanged", "[conformance][roundtrip]") {
    const std::uint32_t channels = 2;
    const std::wstring base = harness::uniqueTestObjectBase(L"roundtrip");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, channels));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::GainProcessor processor(1.0f);
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    TestBlock block(channels, 480);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);

    // Unity gain: the payload survives de-interleave, in-place processing and re-interleave
    // bit-exactly. Nothing here is lossy, so exact equality is the correct assertion.
    REQUIRE(block.output == block.input);

    thread.stop();
    valet.detach();
}

TEST_CASE("planar addressing agrees end to end", "[conformance][roundtrip][planar]") {
    // The valet stamps each sample with its own (channel, frame) position and the king
    // re-interleaves. If the two sides disagreed about `c * perChannel + s` (sec. 4.3), the
    // interleaved result would be channel-smeared rather than obviously wrong.
    const std::uint32_t channels = 6;
    const std::int32_t frames = 128;
    const std::wstring base = harness::uniqueTestObjectBase(L"planar");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, channels));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::StampProcessor processor;
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    TestBlock block(channels, frames);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);

    for (std::int32_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const std::size_t index = static_cast<std::size_t>(frame) * channels + ch;
            REQUIRE(block.output[index] == harness::StampProcessor::expected(ch, frame));
        }
    }

    thread.stop();
    valet.detach();
}

TEST_CASE("gain is applied in place on the shared payload", "[conformance][roundtrip]") {
    const std::uint32_t channels = 2;
    const std::wstring base = harness::uniqueTestObjectBase(L"gain");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(44100, channels));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::GainProcessor processor(0.5f);
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    TestBlock block(channels, 64);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);

    for (std::size_t i = 0; i < block.input.size(); ++i) {
        REQUIRE(block.output[i] == block.input[i] * 0.5f);
    }
    REQUIRE(counters.blocks.load() == 1);

    thread.stop();
    valet.detach();
}

TEST_CASE("a second valet takes the stream over and the incumbent detaches",
          "[conformance][stolen]") {
    const std::wstring base = harness::uniqueTestObjectBase(L"stolen");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));
    king.setValetTimeoutMs(kFastEvictionMs);

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::GainProcessor processor(1.0f);
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    TestBlock block(2, 32);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);

    // Another client arrives and publishes its own id. Displacement is intentional (sec. 4.1).
    const std::uint32_t thiefId = valet.valetId() ^ 0x5A5A5A5Au;
    king.forceValetId(thiefId);

    // The king publishes the next block; our valet must recognise the mismatch and detach
    // (sec. 4.4 step 2). With no real second client to answer, the king then times out -- which is
    // exactly what v1 does in this situation.
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::ValetTimedOut);

    REQUIRE(waitFor([&] { return !thread.running(); }));
    REQUIRE(thread.exitReason() == ipc::ValetExitReason::Stolen);
    thread.stop();

    // The displaced valet must not zero a claim that is no longer its own: doing so would evict
    // an innocent third party. Here the king already evicted the (absent) thief on timeout.
    valet.detach();
    REQUIRE(king.valetIdInHeader() == protocol::kNoValet);
}

TEST_CASE("king-side timeout evicts the valet, which reclaims the stream",
          "[conformance][eviction]") {
    const std::wstring base = harness::uniqueTestObjectBase(L"eviction");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));
    king.setValetTimeoutMs(kFastEvictionMs);

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    // Claim published, but nothing is consuming blocks yet -- the valet thread is not started.
    TestBlock block(2, 32);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::ValetTimedOut);

    // Sec. 4.4 king step 5: on timeout the king writes 0 to valetId and passes audio through.
    REQUIRE(king.valetIdInHeader() == protocol::kNoValet);
    REQUIRE(block.output == block.input);

    // Now start consuming. A zeroed slot is an eviction, not a takeover: nothing else claimed
    // the stream, so the valet re-publishes its id and processing resumes.
    harness::GainProcessor processor(2.0f);
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    REQUIRE(waitFor([&] { return counters.reclaims.load() > 0; }));
    REQUIRE(king.valetIdInHeader() == valet.valetId());
    REQUIRE(thread.running());

    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);
    for (std::size_t i = 0; i < block.input.size(); ++i) {
        REQUIRE(block.output[i] == block.input[i] * 2.0f);
    }

    thread.stop();
    valet.detach();
}

TEST_CASE("a format change that reopens the stream is observed on the next block",
          "[conformance][format]") {
    const std::wstring base = harness::uniqueTestObjectBase(L"format");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::FormatRecorder recorder;
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, recorder, counters);
    thread.start();

    {
        TestBlock block(2, 480);
        REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
                harness::DispatchResult::Processed);
    }
    REQUIRE(recorder.last().sampleRate == 48000);
    REQUIRE(recorder.last().channelCount == 2);
    REQUIRE(recorder.last().frameCount == 480);

    // Both fields differ, so the deployed APO's `&&` condition does trigger a reopen. The valet
    // keeps its claim across it, because the section survives while we hold a handle (sec. 4.5).
    king.smartOpen(44100, 6);
    REQUIRE(king.publishedSampleRate() == 44100);
    REQUIRE(king.publishedChannelCount() == 6);
    REQUIRE(king.valetIdInHeader() == valet.valetId());

    {
        TestBlock block(6, 256);
        REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
                harness::DispatchResult::Processed);
    }

    // The new geometry is picked up on the very next block, with no restart and no reattach,
    // because the header is re-read every block rather than cached (sec. 4.5).
    REQUIRE(recorder.last().sampleRate == 44100);
    REQUIRE(recorder.last().channelCount == 6);
    REQUIRE(recorder.last().frameCount == 256);
    REQUIRE(counters.formatChanges.load() == 2);

    thread.stop();
    valet.detach();
}

TEST_CASE("a sample-rate-only change leaves a stale sampleRate, and the valet copes",
          "[conformance][format][defect]") {
    // This test documents an inherited defect rather than desired behaviour. `smartOpen` in the
    // deployed APO tests `sampleRate != _sampleRate && channelCount != _channelCount` where
    // `||` was meant (sec. 3.7.3), so a sample-rate-only format change does not reopen the stream
    // and the header keeps reporting the *previous* rate.
    //
    // The client cannot detect or fix this -- fixing it requires protocol v2 (sec. 9.1). What
    // it must do is tolerate it: keep processing correctly, and never cache the value
    // (sec. 4.5).
    const std::wstring base = harness::uniqueTestObjectBase(L"stalerate");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::FormatRecorder recorder;
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, recorder, counters);
    thread.start();

    TestBlock block(2, 480);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);
    REQUIRE(recorder.last().sampleRate == 48000);

    king.smartOpen(44100, 2);                           // sample rate only: condition not met
    REQUIRE(king.publishedSampleRate() == 48000);       // still the old value -- the defect
    REQUIRE(king.valetIdInHeader() == valet.valetId()); // no reopen happened

    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);

    // The valet observes the stale rate -- it has no way not to -- but the audio path is intact.
    REQUIRE(recorder.last().sampleRate == 48000);
    REQUIRE(recorder.last().channelCount == 2);
    REQUIRE(recorder.last().frameCount == 480);
    REQUIRE(counters.blocks.load() == 2);
    REQUIRE(counters.malformedBlocks.load() == 0);

    thread.stop();
    valet.detach();
}

TEST_CASE("a malformed header is rejected but the rendezvous still completes",
          "[conformance][malformed]") {
    // The shared objects carry a null DACL (sec. 3.7.2), so any process on the machine can write
    // nonsense into the header. The valet must not fault on it -- and must still signal KING,
    // because leaving the king to time out costs the audio engine's real-time thread up to
    // 1000 ms (sec. 3.7.1).
    const std::wstring base = harness::uniqueTestObjectBase(L"malformed");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));
    king.setValetTimeoutMs(kFastEvictionMs);

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::FormatRecorder recorder;
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, recorder, counters);
    thread.start();

    struct BadHeader {
        const char* what;
        std::uint32_t channelCount;
        std::int32_t size;
    };

    const BadHeader cases[] = {
        {"zero channel count", 0, 480},
        {"negative size", 2, -4},
        {"size beyond the 1 MiB mapping", 2, protocol::kMaxPayloadSamples + 2},
        {"size not divisible by channel count", 6, 481},
    };

    std::uint64_t expectedRejections = 0;
    for (const BadHeader& bad : cases) {
        INFO("malformed case: " << bad.what);
        REQUIRE(king.dispatchRawHeader(48000, bad.channelCount, bad.size) ==
                harness::DispatchResult::Processed);
        ++expectedRejections;
        REQUIRE(waitFor([&] { return counters.malformedBlocks.load() == expectedRejections; }));
    }

    REQUIRE(counters.blocks.load() == 0); // nothing was handed to the processor
    REQUIRE(recorder.count() == 0);
    REQUIRE(thread.running()); // and the valet is still healthy

    // A well-formed block after the garbage is processed normally.
    TestBlock block(2, 32);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);
    REQUIRE(counters.blocks.load() == 1);

    thread.stop();
    valet.detach();
}

TEST_CASE("many consecutive blocks stay in lockstep", "[conformance][roundtrip]") {
    // The rendezvous is a two-event handshake with no sequence number, so a single missed
    // ResetEvent would desynchronise it. Running a few thousand blocks catches that.
    const std::uint32_t channels = 2;
    const std::wstring base = harness::uniqueTestObjectBase(L"lockstep");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, channels));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::GainProcessor processor(2.0f);
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    constexpr int kBlocks = 2000;
    TestBlock block(channels, 128);
    for (int i = 0; i < kBlocks; ++i) {
        REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
                harness::DispatchResult::Processed);
        REQUIRE(block.output[0] == block.input[0] * 2.0f);
        REQUIRE(block.output.back() == block.input.back() * 2.0f);
    }

    REQUIRE(counters.blocks.load() == kBlocks);
    REQUIRE(counters.malformedBlocks.load() == 0);
    REQUIRE(counters.formatChanges.load() == 1); // the initial format, and no change after

    thread.stop();
    valet.detach();
}

TEST_CASE("the supervisor waits for the endpoint and attaches when it appears",
          "[conformance][supervisor]") {
    const std::wstring base = harness::uniqueTestObjectBase(L"supervisor");

    harness::GainProcessor processor(1.0f);
    ipc::SupervisorPolicy policy;
    policy.retryDelayMs = 20;
    ipc::ValetSupervisor supervisor(ipc::ObjectBaseName{base}, processor, policy);

    supervisor.start();
    // Nothing to attach to yet: the endpoint is not active, so the supervisor retries (sec. 4.4).
    REQUIRE(supervisor.state() == ipc::LinkState::Detached);

    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));

    REQUIRE(waitFor([&] { return supervisor.state() == ipc::LinkState::Attached; }));
    REQUIRE(king.valetIdInHeader() != protocol::kNoValet);
    REQUIRE(supervisor.attachCount() == 1);

    TestBlock block(2, 64);
    REQUIRE(king.dispatch(block.input.data(), block.output.data(), block.size) ==
            harness::DispatchResult::Processed);
    REQUIRE(block.output == block.input);

    supervisor.stop();
    REQUIRE(king.valetIdInHeader() == protocol::kNoValet); // stopping detaches cleanly
}

TEST_CASE("the supervisor relinquishes the stream after a takeover",
          "[conformance][supervisor][stolen]") {
    // Sec. 4.1 makes displacement intentional. Re-attaching by default would make two clients
    // ping-pong the endpoint forever, so the supervisor stops instead.
    const std::wstring base = harness::uniqueTestObjectBase(L"relinquish");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));
    king.setValetTimeoutMs(kFastEvictionMs);

    harness::GainProcessor processor(1.0f);
    ipc::SupervisorPolicy policy;
    policy.retryDelayMs = 20;
    ipc::ValetSupervisor supervisor(ipc::ObjectBaseName{base}, processor, policy);
    supervisor.start();

    REQUIRE(waitFor([&] { return supervisor.state() == ipc::LinkState::Attached; }));

    king.forceValetId(0x1234ABCDu);
    TestBlock block(2, 32);
    (void)king.dispatch(block.input.data(), block.output.data(), block.size);

    REQUIRE(waitFor([&] { return supervisor.state() == ipc::LinkState::Relinquished; }));
    REQUIRE(supervisor.attachCount() == 1); // and it does not try again

    supervisor.stop();
}
