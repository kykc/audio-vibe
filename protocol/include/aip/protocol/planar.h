// Protocol v1 -- planar payload addressing and interleave conversion (sec. 4.3).
//
// The payload is de-interleaved (planar) and shared *in place*: the valet reads the king's
// input and writes its output into the same memory. There is no separate output region.
//
//     sample `s` of channel `c` lives at float index `c * perChannel + s`
//     perChannel = size / channelCount
//
// Every function here is allocation-free and safe to call from the audio thread (sec. 7.4.1).

#pragma once

#include "aip/protocol/layout.h"

#include <cassert>
#include <cstdint>

namespace aip::protocol {

/// A non-owning view over the planar payload of one block.
class PlanarView {
public:
    PlanarView() = default;

    PlanarView(float* data, std::uint32_t channelCount, std::int32_t size) noexcept
        : data_(data), channelCount_(channelCount), size_(size),
          perChannel_(channelCount == 0 ? 0 : size / static_cast<std::int32_t>(channelCount)) {}

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr && channelCount_ != 0; }

    [[nodiscard]] std::uint32_t channelCount() const noexcept { return channelCount_; }

    /// Total sample count across all channels -- the `size` field verbatim (sec. 4.3).
    [[nodiscard]] std::int32_t sampleCount() const noexcept { return size_; }

    /// Frames per channel, i.e. `size / channelCount`. The block size in the usual audio sense.
    [[nodiscard]] std::int32_t frameCount() const noexcept { return perChannel_; }

    [[nodiscard]] float* channel(std::uint32_t c) noexcept {
        assert(c < channelCount_);
        return data_ + static_cast<std::ptrdiff_t>(c) * perChannel_;
    }

    [[nodiscard]] const float* channel(std::uint32_t c) const noexcept {
        assert(c < channelCount_);
        return data_ + static_cast<std::ptrdiff_t>(c) * perChannel_;
    }

    [[nodiscard]] float* data() noexcept { return data_; }

    [[nodiscard]] const float* data() const noexcept { return data_; }

private:
    float* data_ = nullptr;
    std::uint32_t channelCount_ = 0;
    std::int32_t size_ = 0;
    std::int32_t perChannel_ = 0;
};

/// Why a block header was rejected. The shared objects carry a null DACL (sec. 3.7.2), so any
/// process can write nonsense into the header; the valet must reject it without undefined
/// behaviour and still complete the rendezvous so the king is not stalled for its full 1000 ms.
enum class HeaderStatus {
    Ok,
    ZeroChannelCount,
    NegativeSize,
    SizeExceedsCapacity,
    SizeNotDivisibleByChannelCount,
};

[[nodiscard]] inline HeaderStatus validateBlock(std::uint32_t channelCount,
                                               std::int32_t size) noexcept {
    if (channelCount == 0) {
        return HeaderStatus::ZeroChannelCount;
    }
    if (size < 0) {
        return HeaderStatus::NegativeSize;
    }
    if (size > kMaxPayloadSamples) {
        return HeaderStatus::SizeExceedsCapacity;
    }
    if (size % static_cast<std::int32_t>(channelCount) != 0) {
        return HeaderStatus::SizeNotDivisibleByChannelCount;
    }
    return HeaderStatus::Ok;
}

/// Interleaved -> planar. `size` is the total sample count (sec. 4.3). This is what the king does
/// when it publishes a block; the conformance harness's synthetic king uses it directly.
inline void deinterleave(const float* from, float* to, std::int32_t size,
                         std::uint32_t channelCount) noexcept {
    const std::int32_t channels = static_cast<std::int32_t>(channelCount);
    const std::int32_t perChannel = size / channels;

    for (std::int32_t ch = 0; ch < channels; ++ch) {
        float* dst = to + static_cast<std::ptrdiff_t>(ch) * perChannel;
        for (std::int32_t s = 0; s < perChannel; ++s) {
            dst[s] = from[s * channels + ch];
        }
    }
}

/// Planar -> interleaved. What the king does when it reads the processed block back.
inline void reinterleave(const float* from, float* to, std::int32_t size,
                         std::uint32_t channelCount) noexcept {
    const std::int32_t channels = static_cast<std::int32_t>(channelCount);
    const std::int32_t perChannel = size / channels;

    for (std::int32_t ch = 0; ch < channels; ++ch) {
        const float* src = from + static_cast<std::ptrdiff_t>(ch) * perChannel;
        for (std::int32_t s = 0; s < perChannel; ++s) {
            to[s * channels + ch] = src[s];
        }
    }
}

} // namespace aip::protocol
