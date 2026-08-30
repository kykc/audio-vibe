#include "aip/engine/output_meter.h"

#include <algorithm>
#include <cmath>

namespace aip::engine {
namespace {

/// BS.1770's absolute offset. It is what makes a 1 kHz sine at -23 dBFS read -23.0 LUFS: the
/// K-weighting's +0.691 dB at 1 kHz and this cancel exactly, which is the calibration EBU Tech
/// 3341's first test signal checks and the test in this suite reproduces.
constexpr double kOffsetDb = -0.691;

/// The analog prototype BS.1770 designs its two sections from. Pasting the standard's 48 kHz
/// coefficient table instead would be shorter and wrong at every other rate -- and this endpoint
/// can be at 44.1, 96 or 192 kHz, and can change while attached (sec. 4.5).
constexpr double kShelfHz = 1681.974450955533;
constexpr double kShelfGainDb = 3.999843853973347;
constexpr double kShelfQ = 0.7071752369554196;
constexpr double kHighPassHz = 38.13547087602444;
constexpr double kHighPassQ = 0.5003270373238773;

/// The exponent relating the shelf's band gain to its high-frequency gain. It is not derived
/// here; it is the value the standard's design uses, and it is what makes the 48 kHz output match
/// the published table to the last digit.
constexpr double kBandGainExponent = 0.4996667741545416;

constexpr double kPi = 3.14159265358979323846;

} // namespace

OutputMeter::OutputMeter() noexcept {
    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        meanSquare_[c].store(0.0f, std::memory_order_relaxed);
        peak_[c].store(0.0f, std::memory_order_relaxed);
    }
}

void OutputMeter::reset(std::uint32_t sampleRate, std::uint32_t channelCount) noexcept {
    const double rate = static_cast<double>(sampleRate);

    {
        const double k = std::tan(kPi * kShelfHz / rate);
        const double vh = std::pow(10.0, kShelfGainDb / 20.0);
        const double vb = std::pow(vh, kBandGainExponent);
        const double a0 = 1.0 + k / kShelfQ + k * k;
        pre_.b0 = (vh + vb * k / kShelfQ + k * k) / a0;
        pre_.b1 = 2.0 * (k * k - vh) / a0;
        pre_.b2 = (vh - vb * k / kShelfQ + k * k) / a0;
        pre_.a1 = 2.0 * (k * k - 1.0) / a0;
        pre_.a2 = (1.0 - k / kShelfQ + k * k) / a0;
    }
    {
        const double k = std::tan(kPi * kHighPassHz / rate);
        const double a0 = 1.0 + k / kHighPassQ + k * k;
        rlb_.b0 = 1.0;
        rlb_.b1 = -2.0;
        rlb_.b2 = 1.0;
        rlb_.a1 = 2.0 * (k * k - 1.0) / a0;
        rlb_.a2 = (1.0 - k / kHighPassQ + k * k) / a0;
    }

    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        preState_[c][0] = preState_[c][1] = 0.0;
        rlbState_[c][0] = rlbState_[c][1] = 0.0;
        for (std::size_t h = 0; h < kSubBlocks; ++h) {
            hops_[c][h] = 0.0;
        }
        partial_[c] = 0.0;
        meanSquare_[c].store(0.0f, std::memory_order_relaxed);
        peak_[c].store(0.0f, std::memory_order_relaxed);
    }
    hopIndex_ = 0;
    hopsFilled_ = 0;
    hopFilledSamples_ = 0;
    // At least one sample, so a rate below 100 Hz -- which no endpoint has, and a corrupt header
    // can claim -- cannot make this a division by zero or an infinite loop below.
    hopSamples_ = std::max<std::int32_t>(1, static_cast<std::int32_t>(sampleRate * kHopMs / 1000));

    preparedRate_ = sampleRate;
    preparedChannels_ = channelCount;
    settled_.store(false, std::memory_order_relaxed);
}

void OutputMeter::closeHop() noexcept {
    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        hops_[c][hopIndex_] = partial_[c];
        partial_[c] = 0.0;
    }
    hopIndex_ = (hopIndex_ + 1) % kSubBlocks;
    hopsFilled_ = std::min(hopsFilled_ + 1, kSubBlocks);
    hopFilledSamples_ = 0;

    // Summed afresh rather than kept as a running total that hops are added to and subtracted
    // from. Forty additions per channel per 10 ms is nothing, and the alternative accumulates
    // rounding for as long as the shell is attached -- which is measured in days here.
    const double window = static_cast<double>(hopsFilled_) * static_cast<double>(hopSamples_);
    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        double sum = 0.0;
        for (std::size_t h = 0; h < hopsFilled_; ++h) {
            sum += hops_[c][h];
        }
        meanSquare_[c].store(static_cast<float>(sum / window), std::memory_order_relaxed);
    }

    if (hopsFilled_ == kSubBlocks) {
        settled_.store(true, std::memory_order_relaxed);
    }
}

void OutputMeter::setEnabled(bool enabled) noexcept {
    if (enabled && !enabled_.load(std::memory_order_relaxed)) {
        // Asked for before the meter is switched on, so the first block after it cannot be folded
        // into a window left over from before it was switched off.
        restart_.store(true, std::memory_order_relaxed);
    }
    enabled_.store(enabled, std::memory_order_relaxed);
}

void OutputMeter::measure(const protocol::PlanarView& audio, std::uint32_t sampleRate) noexcept {
    // First, and before the block counter: nothing is measured, so nothing has been measured, and
    // a reader that looks anyway should see exactly that rather than a frozen level.
    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }
    if (!audio.valid() || sampleRate == 0) {
        return;
    }
    const std::int32_t frames = audio.frameCount();
    if (frames <= 0) {
        return;
    }

    const std::uint32_t channels = audio.channelCount();
    channels_.store(channels, std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);

    // The filter is only correct for the rate it was designed at, and a window measured across a
    // format change describes neither format. Both are rare and both are already transitions
    // sec. 7.4.3 permits to be audible.
    if (sampleRate != preparedRate_ || channels != preparedChannels_ ||
        restart_.exchange(false, std::memory_order_relaxed)) {
        reset(sampleRate, channels);
    }

    const std::size_t metered = std::min<std::size_t>(channelsShownFor(channels), channels);

    // Walked in hop-aligned segments so the 10 ms boundary is found once for the block rather
    // than tested per sample per channel. Bounded: at most frames / hopSamples_ + 1 turns.
    std::int32_t offset = 0;
    while (offset < frames) {
        const std::int32_t take = std::min(hopSamples_ - hopFilledSamples_, frames - offset);

        for (std::size_t c = 0; c < metered; ++c) {
            const float* samples = audio.channel(static_cast<std::uint32_t>(c));
            double sum = 0.0;
            float peak = 0.0f;
            for (std::int32_t s = offset; s < offset + take; ++s) {
                const float sample = samples[s];
                // fabs rather than comparing both signs: one instruction, and it puts a NaN on
                // the losing side of the comparison rather than latching the meter at a value
                // nothing can decay from.
                const float magnitude = std::fabs(sample);
                if (magnitude > peak) {
                    peak = magnitude;
                }
                const double weighted = rlb_.process(pre_.process(sample, preState_[c]), rlbState_[c]);
                sum += weighted * weighted;
            }
            partial_[c] += sum;

            if (peak > peak_[c].load(std::memory_order_relaxed)) {
                peak_[c].store(peak, std::memory_order_relaxed);
            }
        }

        hopFilledSamples_ += take;
        offset += take;
        if (hopFilledSamples_ >= hopSamples_) {
            closeHop();
        }
    }
}

OutputMeter::Reading OutputMeter::read() noexcept {
    Reading reading;
    reading.channels = channels_.load(std::memory_order_relaxed);
    reading.settled = settled_.load(std::memory_order_relaxed);
    reading.blocks = blocks_.load(std::memory_order_relaxed);

    const std::size_t shown = channelsShownFor(reading.channels);
    double summed = 0.0;

    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        const float meanSquare = meanSquare_[c].load(std::memory_order_relaxed);
        reading.peak[c] = peak_[c].exchange(0.0f, std::memory_order_relaxed);
        reading.momentary[c] = meanSquare > 0.0f
            ? static_cast<float>(kOffsetDb + 10.0 * std::log10(static_cast<double>(meanSquare)))
            : kSilentLufs;
        if (c < shown) {
            // G = 1 for both, which is what BS.1770 gives left and right. The weights above 1
            // start at the surrounds, and this build meters no surrounds.
            summed += static_cast<double>(meanSquare);
        }
    }

    reading.programme = summed > 0.0 ? static_cast<float>(kOffsetDb + 10.0 * std::log10(summed)) : kSilentLufs;
    return reading;
}

std::size_t OutputMeter::channelsShownFor(std::uint32_t channelCount) noexcept {
    if (channelCount == 0) {
        return kMaxChannels;
    }
    return channelCount == 1 ? std::size_t{1} : kMaxChannels;
}

float OutputMeter::toDbfs(float peak) noexcept {
    if (!(peak > 0.0f)) {
        // Negated so a NaN lands here rather than in the logarithm.
        return kSilentLufs;
    }
    return 20.0f * std::log10(peak);
}

} // namespace aip::engine
