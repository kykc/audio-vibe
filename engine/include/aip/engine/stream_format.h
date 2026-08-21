// The geometry a plugin chain is built for (design_doc.md sec. 7.4.2).
//
// Everything on the audio path is preallocated at stream open, sized from `sampleRate`,
// `channelCount` and a maximum block size. That triple is this struct, and a chain carries the
// one it was prepared for so the audio thread can compare rather than adapt: a format the chain
// was not built for is passed through untouched and reported to the control thread, which is
// the only place a rebuild may happen (sec. 7.4.3).

#pragma once

#include <cstdint>

namespace aip::engine {

struct StreamFormat {
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    /// Upper bound on frames per channel in one block. The protocol permits far larger blocks
    /// than Windows actually produces (sec. 4.3), so this is a headroom choice, not a limit
    /// the king agreed to -- a block past it is passed through, never truncated.
    std::int32_t maxFrames = 0;

    [[nodiscard]] bool valid() const noexcept {
        return sampleRate != 0 && channelCount != 0 && maxFrames > 0;
    }

    [[nodiscard]] friend bool operator==(const StreamFormat&, const StreamFormat&) = default;
};

/// Default headroom over the 480 frames the Windows engine uses at 48 kHz. Large enough that an
/// endpoint at 192 kHz with a 10 ms period still fits, small enough that the scratch banks stay
/// in cache-friendly territory.
inline constexpr std::int32_t kDefaultMaxFrames = 4096;

/// Ceiling on channel count. Protocol v1's 1 MiB mapping allows more, but a chain sized for an
/// implausible channel count would waste memory that must be resident and touched.
inline constexpr std::uint32_t kMaxChannels = 16;

} // namespace aip::engine
