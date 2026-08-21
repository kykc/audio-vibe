// Polling helper for the conformance tests. The valet runs on its own thread, so assertions
// about what it has observed are inherently "eventually" assertions; this keeps the deadline in
// one place rather than sprinkling sleeps through the suite.

#pragma once

#include <chrono>
#include <thread>

namespace aip::harness {

/// Default deadline. Generous relative to ValetThread::kBlockWaitMs (100 ms) so that a loaded
/// CI machine does not produce flakes, but short enough that a genuine hang fails the suite.
inline constexpr std::chrono::milliseconds kDefaultWait{3000};

template <typename Predicate>
[[nodiscard]] bool waitFor(Predicate predicate,
                           std::chrono::milliseconds timeout = kDefaultWait) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace aip::harness
