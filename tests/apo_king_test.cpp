// Conformance of the rewritten APO's king half (design_doc.md sec. 4.7).
//
// `apo::BufferKing` is driven directly rather than through the DLL, and against the *production*
// `ipc::BufferValet` rather than a stand-in. That combination is the point: the pair under test
// is the pair that has to work on a real machine, and neither side of it was written to make the
// other pass.
//
// What is deliberately not here is anything that needs the DLL loaded, `audiodg.exe`, or an
// elevated process. Creating a `Global\` object needs SeCreateGlobalPrivilege, which a test
// runner does not hold (status.md sec. 7 item 5), so these use `Local\` names -- the exact sec. 4.2
// name construction has its own test in `conformance_test.cpp`, and the DLL end to end is
// `tools/apo_host`.

#include <catch2/catch_test_macros.hpp>

#include "aip/apo/buffer_king.h"
#include "aip/ipc/buffer_valet.h"
#include "aip/protocol/planar.h"
#include "harness/valet_driver.h"
#include "harness/wait_for.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

using namespace aip;

namespace {

/// A unique `Local\` base name per test, so cases cannot collide with each other or with a
/// previous run's leftovers.
std::wstring uniqueBase(const wchar_t* tag) {
    static std::atomic<int> counter{0};
    return std::wstring(L"Local\\AIP.APO.KING.TEST.")
        .append(tag)
        .append(L".")
        .append(std::to_wstring(::GetCurrentProcessId()))
        .append(L".")
        .append(std::to_wstring(counter.fetch_add(1)));
}

/// An interleaved ramp, distinct per channel, so that a de-interleave that transposes channels or
/// loses a sample cannot pass.
std::vector<float> rampBlock(std::int32_t frames, std::uint32_t channels) {
    std::vector<float> block(static_cast<std::size_t>(frames) * channels);
    for (std::int32_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            block[static_cast<std::size_t>(f) * channels + c] = static_cast<float>(c) * 1000.0f + static_cast<float>(f);
        }
    }
    return block;
}

} // namespace

TEST_CASE("the king passes audio through untouched when no valet is attached", "[apo][king]") {
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"novalet");
    REQUIRE(king.open(base, 48000, 2));

    auto input = rampBlock(480, 2);
    std::vector<float> output(input.size(), -1.0f);

    REQUIRE(king.dispatch(input.data(), output.data(), static_cast<std::int32_t>(input.size())) ==
        apo::DispatchResult::NoValet);

    // `NoValet` leaves the copy to the caller -- the APO does it, because with APO_FLAG_INPLACE
    // there is usually nothing to copy. So the output is untouched here, and that is correct.
    REQUIRE(output[0] == -1.0f);
}

TEST_CASE("a block survives the round trip to a valet and back", "[apo][king]") {
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"roundtrip");
    REQUIRE(king.open(base, 44100, 2));

    harness::ValetDriver valet;
    REQUIRE(valet.start(base));

    auto input = rampBlock(256, 2);
    std::vector<float> output(input.size(), 0.0f);

    // The first dispatch can race the valet's own attach, so allow a couple of attempts. A king
    // that needed more than that would be failing, and the `REQUIRE` below says so.
    apo::DispatchResult result = apo::DispatchResult::NoValet;
    for (int attempt = 0; attempt < 20 && result != apo::DispatchResult::Processed; ++attempt) {
        result = king.dispatch(input.data(), output.data(), static_cast<std::int32_t>(input.size()));
    }
    REQUIRE(result == apo::DispatchResult::Processed);

    // Interleaved in, planar across the wire, interleaved out -- bit for bit.
    REQUIRE(output == input);

    REQUIRE(valet.lastSampleRate() == 44100);
    REQUIRE(valet.lastChannelCount() == 2);
    REQUIRE(valet.lastFrameCount() == 256);

    valet.stop();
}

TEST_CASE("what the valet writes is what the king reads back", "[apo][king]") {
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"transform");
    REQUIRE(king.open(base, 48000, 2));

    harness::ValetDriver valet;
    // Negate channel 0 only. A transform that treated the payload as interleaved would corrupt
    // both channels, which the assertions below would catch.
    valet.setTransform([](protocol::PlanarView& audio) {
        float* left = audio.channel(0);
        for (std::int32_t s = 0; s < audio.frameCount(); ++s) {
            left[s] = -left[s];
        }
    });
    REQUIRE(valet.start(base));

    auto input = rampBlock(128, 2);
    std::vector<float> output(input.size(), 0.0f);

    apo::DispatchResult result = apo::DispatchResult::NoValet;
    for (int attempt = 0; attempt < 20 && result != apo::DispatchResult::Processed; ++attempt) {
        std::fill(output.begin(), output.end(), 0.0f);
        result = king.dispatch(input.data(), output.data(), static_cast<std::int32_t>(input.size()));
    }
    REQUIRE(result == apo::DispatchResult::Processed);

    for (std::int32_t f = 0; f < 128; ++f) {
        REQUIRE(output[static_cast<std::size_t>(f) * 2 + 0] == -input[static_cast<std::size_t>(f) * 2 + 0]);
        REQUIRE(output[static_cast<std::size_t>(f) * 2 + 1] == input[static_cast<std::size_t>(f) * 2 + 1]);
    }

    valet.stop();
}

TEST_CASE("a valet that misses the deadline is evicted and can reclaim", "[apo][king]") {
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"evict");
    REQUIRE(king.open(base, 48000, 2));

    harness::ValetDriver valet;
    REQUIRE(valet.start(base));

    auto input = rampBlock(64, 2);
    std::vector<float> output(input.size(), 0.0f);
    const auto size = static_cast<std::int32_t>(input.size());

    // Settle first: one clean block, so what follows is unambiguous.
    apo::DispatchResult result = apo::DispatchResult::NoValet;
    for (int attempt = 0; attempt < 20 && result != apo::DispatchResult::Processed; ++attempt) {
        result = king.dispatch(input.data(), output.data(), size);
    }
    REQUIRE(result == apo::DispatchResult::Processed);

    // The real king waits a full second (sec. 4.4 step 5) and reproducing that here would cost a
    // second per case, so the *valet* is made late instead of the king impatient -- the timeout
    // itself stays at the shipping value, which is the number worth not diverging from.
    valet.stallNextBlocks(1, apo::BufferKing::kValetTimeoutMs + 400);

    REQUIRE(king.dispatch(input.data(), output.data(), size) == apo::DispatchResult::ValetTimedOut);
    REQUIRE(king.evictionCount() == 1);

    // Eviction is recoverable and must be: nothing else claimed the stream, so the valet
    // re-claims and audio resumes. A king that treated this as terminal would drop a client for
    // one late block (status.md sec. 7 item 2).
    REQUIRE(harness::waitFor(
        [&] { return king.dispatch(input.data(), output.data(), size) == apo::DispatchResult::Processed; }));

    valet.stop();
}

TEST_CASE("a second valet takes the stream and the first stands down", "[apo][king]") {
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"steal");
    REQUIRE(king.open(base, 48000, 2));

    harness::ValetDriver first;
    REQUIRE(first.start(base));

    auto input = rampBlock(64, 2);
    std::vector<float> output(input.size(), 0.0f);
    const auto size = static_cast<std::int32_t>(input.size());

    apo::DispatchResult result = apo::DispatchResult::NoValet;
    for (int attempt = 0; attempt < 20 && result != apo::DispatchResult::Processed; ++attempt) {
        result = king.dispatch(input.data(), output.data(), size);
    }
    REQUIRE(result == apo::DispatchResult::Processed);

    // Displacement is intentional (sec. 4.1): the newcomer simply writes its own id.
    harness::ValetDriver second;
    REQUIRE(second.start(base));

    REQUIRE(harness::waitFor([&] {
        (void)king.dispatch(input.data(), output.data(), size);
        return first.stolen() > 0;
    }));

    // And the king keeps working, now serving the newcomer.
    REQUIRE(harness::waitFor(
        [&] { return king.dispatch(input.data(), output.data(), size) == apo::DispatchResult::Processed; }));

    first.stop();
    second.stop();
}

TEST_CASE("the rewritten king does not have the smartOpen bug", "[apo][king]") {
    // The deployed APO tests `sampleRate != _sampleRate && channelCount != _channelCount` where
    // `||` was meant (sec. 3.7.3), so a sample-rate-only change leaves a stale rate in the
    // header. `SyntheticKing` still reproduces that, because the client must tolerate the
    // deployed binary -- and there is a test asserting it does. This is the other half of that
    // pair: the *new* king must not.
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"smartopen");
    REQUIRE(king.smartOpen(base, 44100, 2));
    REQUIRE(king.sampleRate() == 44100);

    // Sample rate only. Channel count deliberately unchanged: that is precisely the case the
    // `&&` misses.
    REQUIRE(king.smartOpen(base, 48000, 2));
    REQUIRE(king.sampleRate() == 48000);

    harness::ValetDriver valet;
    REQUIRE(valet.start(base));

    auto input = rampBlock(64, 2);
    std::vector<float> output(input.size(), 0.0f);
    const auto size = static_cast<std::int32_t>(input.size());

    REQUIRE(harness::waitFor(
        [&] { return king.dispatch(input.data(), output.data(), size) == apo::DispatchResult::Processed; }));

    // What the valet actually saw on the wire, which is the only claim that matters.
    REQUIRE(valet.lastSampleRate() == 48000);

    valet.stop();
}

TEST_CASE("an unchanged format does not reopen the stream", "[apo][king]") {
    // The other side of `smartOpen`: reopening on every `LockForProcess` would tear down the
    // objects under an attached valet and make it re-attach for nothing.
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"noreopen");
    REQUIRE(king.smartOpen(base, 48000, 2));

    harness::ValetDriver valet;
    REQUIRE(valet.start(base));

    auto input = rampBlock(64, 2);
    std::vector<float> output(input.size(), 0.0f);
    const auto size = static_cast<std::int32_t>(input.size());

    REQUIRE(harness::waitFor(
        [&] { return king.dispatch(input.data(), output.data(), size) == apo::DispatchResult::Processed; }));

    REQUIRE(king.smartOpen(base, 48000, 2));

    // Still the same valet, still attached, no re-attach needed.
    REQUIRE(king.dispatch(input.data(), output.data(), size) == apo::DispatchResult::Processed);

    valet.stop();
}

TEST_CASE("reopening an existing section does not evict an attached valet", "[apo][king]") {
    // Sec. 4.5: `valetId` is zeroed only when the section was created fresh. A king that zeroed
    // unconditionally would throw away a client that had done nothing wrong, every time the
    // endpoint's format changed.
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"reopen");
    REQUIRE(king.open(base, 48000, 2));

    harness::ValetDriver valet;
    REQUIRE(valet.start(base));

    auto input = rampBlock(64, 2);
    std::vector<float> output(input.size(), 0.0f);
    const auto size = static_cast<std::int32_t>(input.size());

    REQUIRE(harness::waitFor(
        [&] { return king.dispatch(input.data(), output.data(), size) == apo::DispatchResult::Processed; }));

    // A second king on the same name, as a format change effectively is. The valet holds the
    // section open, so this attaches rather than creates.
    apo::BufferKing other;
    REQUIRE(other.open(base, 96000, 2));

    auto wide = rampBlock(64, 2);
    std::vector<float> wideOut(wide.size(), 0.0f);
    REQUIRE(other.dispatch(wide.data(), wideOut.data(), static_cast<std::int32_t>(wide.size())) ==
        apo::DispatchResult::Processed);

    valet.stop();
}

TEST_CASE("a block geometry protocol v1 cannot carry is refused, not truncated", "[apo][king]") {
    apo::BufferKing king;
    const std::wstring base = uniqueBase(L"geometry");
    REQUIRE(king.open(base, 48000, 2));

    std::vector<float> buffer(1024, 0.0f);

    // Larger than the 1 MiB mapping can hold (sec. 4.3). Publishing it would write past the
    // view, inside audiodg.exe.
    REQUIRE(
        king.dispatch(buffer.data(), buffer.data(), protocol::kMaxPayloadSamples + 2) == apo::DispatchResult::Unusable);

    // Not a whole number of frames.
    REQUIRE(king.dispatch(buffer.data(), buffer.data(), 101) == apo::DispatchResult::Unusable);

    // Negative.
    REQUIRE(king.dispatch(buffer.data(), buffer.data(), -8) == apo::DispatchResult::Unusable);
}

TEST_CASE("a king with no stream open passes through rather than faulting", "[apo][king]") {
    // `LockForProcess` tolerates a failed `smartOpen` and lets the APO run as a pass-through
    // rather than failing the endpoint. This is the state that reaches the audio thread when it
    // does.
    apo::BufferKing king;
    std::vector<float> buffer(256, 0.5f);
    REQUIRE(king.dispatch(buffer.data(), buffer.data(), 256) == apo::DispatchResult::Unusable);
    REQUIRE_FALSE(king.opened());
}
