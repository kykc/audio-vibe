// The consumer half of the protocol conformance harness -- design_doc.md sec. 4.7's "synthetic
// valet", the thing that exists so a rewritten APO can be exercised without a shell attached.
//
// **It is a driver over the production `BufferValet`, not a second implementation of one**, and
// that is a deliberate choice worth defending. `SyntheticKing` had to be written from scratch
// because the king it stands in for lives in `audiodg.exe` and is not our code -- and because it
// has to reproduce that king's bugs. Neither is true here: the valet the APO must interoperate
// with *is* `ipc::BufferValet`, it is in this repository, and a parallel implementation would be
// a second thing to keep correct and a first thing to drift. Testing the king against a hand-made
// valet would prove the king works with the hand-made valet.
//
// What was actually missing was not an implementation but a way to *misbehave on cue*. A king has
// to be tested against a valet that is slow enough to be evicted, one that walks off mid-block,
// and one that another valet steals the stream from. All three are scripted here, on top of the
// real thing.

#pragma once

#include "aip/ipc/buffer_valet.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace aip::harness {

/// Runs a real `BufferValet` on its own thread and applies a transform to every block.
class ValetDriver {
public:
    /// Called for each captured block, with the planar payload in place (sec. 4.3). Runs on the
    /// driver's thread, between acquire and release, which is exactly where a real valet's plugin
    /// chain runs. Default: pass through untouched.
    using Transform = std::function<void(protocol::PlanarView&)>;

    ValetDriver() = default;

    ~ValetDriver() { stop(); }

    ValetDriver(const ValetDriver&) = delete;
    ValetDriver& operator=(const ValetDriver&) = delete;

    void setTransform(Transform transform) { transform_ = std::move(transform); }

    /// Stall for `ms` inside the *next* `count` blocks, before releasing. Longer than the king's
    /// timeout and the king evicts us, which is the only way to reach that branch on purpose.
    void stallNextBlocks(int count, unsigned ms) noexcept {
        stallBlocks_.store(count, std::memory_order_relaxed);
        stallMs_.store(ms, std::memory_order_relaxed);
    }

    /// Attaches and starts the block loop. Returns false if the objects do not exist, i.e. no
    /// king has opened the stream yet.
    [[nodiscard]] bool start(const std::wstring& objectBase);

    /// Stops the loop, detaches cleanly, and joins. Idempotent.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::uint64_t captured() const noexcept { return captured_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::uint64_t timeouts() const noexcept { return timeouts_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::uint64_t reclaims() const noexcept { return reclaims_.load(std::memory_order_relaxed); }

    [[nodiscard]] std::uint64_t stolen() const noexcept { return stolen_.load(std::memory_order_relaxed); }

    /// Geometry of the most recently captured block, straight from the header. This is what
    /// proves a king published the format it claimed to -- including, for the deployed king's
    /// `smartOpen` bug, that it published a stale one.
    [[nodiscard]] std::uint32_t lastSampleRate() const noexcept {
        return lastSampleRate_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint32_t lastChannelCount() const noexcept {
        return lastChannelCount_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::int32_t lastFrameCount() const noexcept {
        return lastFrameCount_.load(std::memory_order_relaxed);
    }

private:
    void run();

    ipc::BufferValet valet_;
    std::thread thread_;
    Transform transform_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<int> stallBlocks_{0};
    std::atomic<unsigned> stallMs_{0};

    std::atomic<std::uint64_t> captured_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> reclaims_{0};
    std::atomic<std::uint64_t> stolen_{0};
    std::atomic<std::uint32_t> lastSampleRate_{0};
    std::atomic<std::uint32_t> lastChannelCount_{0};
    std::atomic<std::int32_t> lastFrameCount_{0};
};

} // namespace aip::harness
