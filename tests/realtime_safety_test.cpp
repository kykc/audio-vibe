// Real-time safety enforcement (design_doc.md sec. 7.4.3, sec. 7.4.6).
//
// Sec. 7.4.3 draws the line that governs this suite: user-initiated transitions may click, but
// steady state must be completely inert -- zero allocations, zero frees, no lock acquisition, no
// growth in resident set. That is directly testable, and sec. 7.4.6 says to enforce it as such.
//
// The first two tests validate the instrument itself. A soak test that reports zero allocations
// is worthless if the detector is not actually wired up, so we prove it can see a violation
// before trusting it to report their absence.

#include "harness/synthetic_king.h"
#include "harness/test_processors.h"
#include "harness/wait_for.h"

#include "aip/ipc/buffer_valet.h"
#include "aip/ipc/valet_thread.h"
#include "aip/rt/mutex.h"
#include "aip/rt/realtime_guard.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <psapi.h>

#include <memory>
#include <numeric>
#include <vector>

using namespace aip;

namespace {

std::size_t workingSetBytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0;
    }
    return counters.WorkingSetSize;
}

} // namespace

TEST_CASE("the detector sees an allocation inside a real-time section", "[rt][detector]") {
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    rt::resetViolations();
    REQUIRE(rt::violations().total() == 0);
    REQUIRE_FALSE(rt::inRealtimeSection());

    // Outside a section, allocation is ordinary control-plane work and must not be counted.
    { volatile auto outside = std::make_unique<int>(1); (void)outside; }
    REQUIRE(rt::violations().total() == 0);

    {
        const rt::RealtimeGuard guard;
        REQUIRE(rt::inRealtimeSection());
        volatile auto inside = std::make_unique<int>(2);
        (void)inside;
    }

    const rt::ViolationCounts counts = rt::violations();
    REQUIRE(counts.allocations == 1);
    REQUIRE(counts.deallocations == 1); // the unique_ptr freed inside the section too
    rt::resetViolations();
}

TEST_CASE("the detector sees a lock taken inside a real-time section", "[rt][detector]") {
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    rt::resetViolations();
    rt::Mutex mutex;

    {
        // Control thread: allowed, not counted.
        const rt::ScopedLock lock(mutex);
    }
    REQUIRE(rt::violations().locks == 0);

    {
        const rt::RealtimeGuard guard;
        const rt::ScopedLock lock(mutex);
    }
    REQUIRE(rt::violations().locks == 1);
    rt::resetViolations();
}

TEST_CASE("a probe counts what a real-time section did without charging the process",
          "[rt][probe]") {
    // The whole value of `rt::ViolationProbe` is the second half of that sentence. Deliberately
    // exercising third-party code that is expected to allocate -- which is what the engine's
    // plugin warm-up does -- must not move the counters sec. 7.4.3 makes an acceptance criterion,
    // or that criterion quietly starts meaning something else.
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    rt::resetViolations();

    rt::ViolationCounts seen;
    {
        const rt::ViolationProbe probe;
        // Inside a probe the thread *is* in a real-time section -- that is what makes the
        // allocation below observable at all.
        CHECK(rt::inRealtimeSection());
        auto leaked = std::make_unique<int>(7);
        CHECK(*leaked == 7);
        seen = probe.counts();
    }

    CHECK(seen.allocations == 1);
    CHECK(rt::violations().total() == 0);
    CHECK_FALSE(rt::inRealtimeSection());
}

TEST_CASE("a probe suspends the global counters and hands them back", "[rt][probe]") {
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    rt::resetViolations();
    {
        const rt::RealtimeGuard guard;
        {
            const rt::ViolationProbe probe;
            auto diverted = std::make_unique<int>(1);
            CHECK(*diverted == 1);
        }
        // Back in the plain guard: this one is the process's problem again, which is what makes
        // the diversion a suspension rather than a switch that stays flipped.
        auto charged = std::make_unique<int>(2);
        CHECK(*charged == 2);
    }
    CHECK(rt::violations().allocations == 1);
    rt::resetViolations();
}

TEST_CASE("steady state performs exactly zero audio-thread allocations", "[rt][soak]") {
    // The sec. 7.4.3 acceptance criterion, verbatim: "A chain that has been running untouched for
    // hours must show the same audio-thread allocation count and the same resident set as it did
    // one second after it started." Hours compress to thousands of blocks here; the assertion is
    // the same one.
    if constexpr (!rt::checksEnabled()) {
        SKIP("built without AIP_RT_CHECKS (Release); the detector is compiled out by design");
    }

    constexpr std::uint32_t kChannels = 2;
    constexpr std::int32_t kFrames = 128;
    constexpr std::int32_t kSize = kFrames * static_cast<std::int32_t>(kChannels);
    constexpr int kWarmupBlocks = 500;
    constexpr int kSoakBlocks = 20000;

    const std::wstring base = harness::uniqueTestObjectBase(L"soak");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, kChannels));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    // Inert on purpose: any allocation the detector reports is then unambiguously ours, from the
    // valet loop or the protocol plumbing, not from a processor standing in for a plugin chain.
    harness::InertProcessor processor;
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    std::vector<float> input(kSize);
    std::vector<float> output(kSize);
    std::iota(input.begin(), input.end(), 1.f);

    // Warm up: first-touch page faults, thread start-up and lazy CRT initialisation all belong
    // to attach, not to steady state.
    for (int i = 0; i < kWarmupBlocks; ++i) {
        REQUIRE(king.dispatch(input.data(), output.data(), kSize) ==
                harness::DispatchResult::Processed);
    }

    rt::resetViolations();
    const std::size_t rssBefore = workingSetBytes();

    for (int i = 0; i < kSoakBlocks; ++i) {
        if (king.dispatch(input.data(), output.data(), kSize) !=
            harness::DispatchResult::Processed) {
            FAIL("block " << i << " did not complete the rendezvous");
        }
    }

    const rt::ViolationCounts counts = rt::violations();
    const std::size_t rssAfter = workingSetBytes();

    INFO("blocks processed: " << counters.blocks.load());
    INFO("working set before: " << rssBefore << " after: " << rssAfter);

    // Exactly zero. Not "few", not "bounded" -- sec. 7.4.3 is explicit about this.
    CHECK(counts.allocations == 0);
    CHECK(counts.deallocations == 0);
    CHECK(counts.locks == 0);

    // Flat resident set. The tolerance covers unrelated process-wide noise (Catch2's own
    // bookkeeping on this thread, the working-set trimmer), not growth in the audio path -- a
    // real leak at one allocation per block would be orders of magnitude past it.
    constexpr std::size_t kRssToleranceBytes = 512u * 1024u;
    CHECK(rssAfter <= rssBefore + kRssToleranceBytes);

    CHECK(counters.blocks.load() >= static_cast<std::uint64_t>(kWarmupBlocks + kSoakBlocks));
    CHECK(counters.malformedBlocks.load() == 0);

    thread.stop();
    valet.detach();
}

TEST_CASE("the valet thread takes Pro Audio characteristics", "[rt][priority]") {
    // Sec. 4.6 is behavioural rather than wire-level, but it is required for glitch-free operation:
    // the king blocks the audio engine's own real-time thread waiting for us (sec. 3.7.1).
    const std::wstring base = harness::uniqueTestObjectBase(L"priority");
    harness::SyntheticKing king(base);
    REQUIRE(king.open(48000, 2));

    ipc::BufferValet valet;
    REQUIRE(valet.attach(base));

    harness::InertProcessor processor;
    ipc::ValetCounters counters;
    ipc::ValetThread thread(valet, processor, counters);
    thread.start();

    REQUIRE(harness::waitFor([&] { return thread.mmcssActive(); }));

    thread.stop();
    valet.detach();
}
