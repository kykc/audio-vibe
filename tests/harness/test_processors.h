// Block processors used by the conformance harness (design_doc.md sec. 4.7).
//
// Every one of these runs on the promoted valet thread, so every one of them obeys sec. 7.4.1: no
// heap, no locks, no I/O, fixed-capacity storage only. The real-time soak test asserts exactly
// that, so a processor here that allocated would show up as a failure rather than as a mystery.

#pragma once

#include "aip/ipc/valet_thread.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace aip::harness {

/// Scales every sample by a gain the control thread can change at any time. The atomic load is
/// the whole point: this is the smallest example of the sec. 7.4.3 pattern where the audio thread
/// only ever reads a value the control thread published.
class GainProcessor final : public ipc::BlockProcessor {
public:
    explicit GainProcessor(float gain = 1.0f) : gain_(gain) {}

    void setGain(float gain) noexcept { gain_.store(gain, std::memory_order_relaxed); }

    void processBlock(ipc::BlockInfo& block) noexcept override {
        const float gain = gain_.load(std::memory_order_relaxed);
        const std::int32_t frames = block.audio.frameCount();
        for (std::uint32_t ch = 0; ch < block.channelCount; ++ch) {
            float* samples = block.audio.channel(ch);
            for (std::int32_t s = 0; s < frames; ++s) {
                samples[s] *= gain;
            }
        }
    }

private:
    std::atomic<float> gain_;
};

/// Overwrites every sample with a value derived from its (channel, frame) position. This pins
/// down the planar addressing agreement of sec. 4.3 end to end: if the valet and the king disagreed
/// about `c * perChannel + s`, the king's re-interleaved output would not match.
class StampProcessor final : public ipc::BlockProcessor {
public:
    static float expected(std::uint32_t channel, std::int32_t frame) noexcept {
        return static_cast<float>(channel) * 100000.f + static_cast<float>(frame);
    }

    void processBlock(ipc::BlockInfo& block) noexcept override {
        const std::int32_t frames = block.audio.frameCount();
        for (std::uint32_t ch = 0; ch < block.channelCount; ++ch) {
            float* samples = block.audio.channel(ch);
            for (std::int32_t s = 0; s < frames; ++s) {
                samples[s] = expected(ch, s);
            }
        }
    }
};

/// Records the geometry the header reported for each block, so tests can assert on what the
/// valet *observed* rather than on what the king believes it published. Fixed-capacity ring --
/// it drops rather than grows (sec. 7.4.2).
class FormatRecorder final : public ipc::BlockProcessor {
public:
    struct Observation {
        std::uint32_t sampleRate = 0;
        std::uint32_t channelCount = 0;
        std::int32_t frameCount = 0;
    };

    static constexpr std::size_t kCapacity = 64;

    void processBlock(ipc::BlockInfo& block) noexcept override {
        const std::size_t index = count_.fetch_add(1, std::memory_order_relaxed);
        slots_[index % kCapacity] = Observation{block.sampleRate, block.channelCount, block.audio.frameCount()};
    }

    [[nodiscard]] std::size_t count() const noexcept { return count_.load(std::memory_order_acquire); }

    /// The most recently recorded observation. Only meaningful once `count() > 0`.
    [[nodiscard]] Observation last() const noexcept {
        const std::size_t seen = count_.load(std::memory_order_acquire);
        return seen == 0 ? Observation{} : slots_[(seen - 1) % kCapacity];
    }

private:
    std::array<Observation, kCapacity> slots_{};
    std::atomic<std::size_t> count_{0};
};

/// Does nothing at all. The baseline for the allocation soak test: with this installed, any
/// audio-thread allocation the detector reports comes from our own plumbing.
using InertProcessor = ipc::PassThroughProcessor;

} // namespace aip::harness
