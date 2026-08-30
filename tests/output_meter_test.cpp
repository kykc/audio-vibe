// The output loudness meter (engine/output_meter.h): ITU-R BS.1770 K-weighting and EBU R128
// momentary loudness, measured on the audio thread.
//
// A meter is a thing that is either calibrated or decorative, and the difference is not visible
// by looking at it -- a wrong constant, a filter designed at the wrong rate, or a window one hop
// short all produce a bar that moves plausibly with the music and reads the wrong number. So the
// central test here is not ours: **EBU Tech 3341's first test signal**, a stereo 1 kHz sine at
// -23 dBFS, which the standard requires to read -23.0 LUFS to within 0.1. Everything else in this
// file is a property that follows from getting that right.
//
// It is run at three sample rates on purpose. The standard publishes its coefficient table at
// 48 kHz, and a meter that pasted that table rather than deriving the filter passes at 48 kHz and
// fails at 44.1 -- which is the rate a lot of this machine's audio is actually at.

#include "aip/engine/chain_processor.h"
#include "aip/engine/output_meter.h"
#include "aip/protocol/planar.h"
#include "aip/rt/realtime_guard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace aip;
using Catch::Approx;

namespace {

using engine::OutputMeter;

constexpr double kPi = 3.14159265358979323846;

/// The amplitude of a sine whose *peak* is `dbfs` below full scale. Tech 3341 specifies its test
/// signals this way, and the distinction matters: the same sine described by its RMS is 3 dB
/// along from here and reads 3 LU differently.
double peakAmplitude(double dbfs) { return std::pow(10.0, dbfs / 20.0); }

/// Feeds `seconds` of a sine into `meter`, in blocks of `frames`, on the channels named in
/// `driven` and silence on the rest. Phase is continuous across blocks, because a meter fed a
/// signal that restarts every block measures the discontinuities as much as the tone.
void feedSine(OutputMeter& meter, std::uint32_t sampleRate, std::uint32_t channels,
    const std::vector<double>& amplitudePerChannel, double frequency = 1000.0, double seconds = 1.0,
    std::int32_t frames = 480) {
    std::vector<float> samples(static_cast<std::size_t>(channels) * static_cast<std::size_t>(frames));
    const std::int32_t total = static_cast<std::int32_t>(seconds * sampleRate);

    for (std::int32_t start = 0; start + frames <= total; start += frames) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const double amplitude = c < amplitudePerChannel.size() ? amplitudePerChannel[c] : 0.0;
            for (std::int32_t s = 0; s < frames; ++s) {
                const double phase = 2.0 * kPi * frequency * static_cast<double>(start + s) / sampleRate;
                samples[static_cast<std::size_t>(c) * static_cast<std::size_t>(frames) + static_cast<std::size_t>(s)] =
                    static_cast<float>(amplitude * std::sin(phase));
            }
        }
        protocol::PlanarView view(samples.data(), channels, static_cast<std::int32_t>(samples.size()));
        meter.measure(view, sampleRate);
    }
}

/// One planar block of constant magnitude, for the peak tests, where a tone would only get in
/// the way.
class Block {
public:
    Block(std::uint32_t channels, std::int32_t frames)
        : samples_(static_cast<std::size_t>(channels) * static_cast<std::size_t>(frames)), channels_(channels),
          frames_(frames) {}

    void set(std::uint32_t channel, float value) noexcept {
        for (std::int32_t s = 0; s < frames_; ++s) {
            samples_[static_cast<std::size_t>(channel) * static_cast<std::size_t>(frames_) +
                static_cast<std::size_t>(s)] = value;
        }
    }

    [[nodiscard]] float at(std::uint32_t channel, std::int32_t frame) const noexcept {
        return samples_[static_cast<std::size_t>(channel) * static_cast<std::size_t>(frames_) +
            static_cast<std::size_t>(frame)];
    }

    protocol::PlanarView view() noexcept {
        return protocol::PlanarView(samples_.data(), channels_, static_cast<std::int32_t>(samples_.size()));
    }

    [[nodiscard]] std::int32_t frames() const noexcept { return frames_; }

private:
    std::vector<float> samples_;
    std::uint32_t channels_;
    std::int32_t frames_;
};

} // namespace

TEST_CASE("EBU Tech 3341 test 1: a stereo 1 kHz sine at -23 dBFS reads -23 LUFS", "[engine][meter]") {
    // The calibration the whole class stands on. It works because BS.1770's -0.691 offset and the
    // K-weighting's +0.691 dB at 1 kHz cancel exactly -- so this one number checks the filter, the
    // window, the channel sum and the constant at once, and it is not a number of ours.
    const std::uint32_t rate = GENERATE(std::uint32_t{48000}, std::uint32_t{44100}, std::uint32_t{96000});

    OutputMeter meter;
    feedSine(meter, rate, 2, {peakAmplitude(-23.0), peakAmplitude(-23.0)});

    const OutputMeter::Reading reading = meter.read();
    INFO("sample rate " << rate);
    CHECK(reading.settled);
    // The tolerance is the standard's own for this signal.
    CHECK(reading.programme == Approx(-23.0).margin(0.1));
    CHECK(reading.momentary[0] == Approx(-26.0).margin(0.1));
    CHECK(reading.momentary[1] == Approx(-26.0).margin(0.1));
}

TEST_CASE("one channel of the pair reads 3 LU below the two together", "[engine][meter]") {
    // BS.1770 sums the channels rather than averaging them, so a silent right channel costs
    // exactly 3.01 LU and not half the reading. This is the property that makes a two-bar meter
    // and a programme figure disagree by a known amount rather than by an unexplained one.
    OutputMeter meter;
    feedSine(meter, 48000, 2, {peakAmplitude(-23.0), 0.0});

    const OutputMeter::Reading reading = meter.read();
    CHECK(reading.programme == Approx(-26.0).margin(0.1));
    CHECK(reading.momentary[0] == Approx(-26.0).margin(0.1));
    CHECK(reading.momentary[1] == OutputMeter::kSilentLufs);
}

TEST_CASE("a mono stream is metered on its one channel", "[engine][meter]") {
    OutputMeter meter;
    feedSine(meter, 48000, 1, {peakAmplitude(-23.0)});

    const OutputMeter::Reading reading = meter.read();
    CHECK(reading.channels == 1);
    CHECK(OutputMeter::channelsShownFor(reading.channels) == 1);
    CHECK(reading.programme == Approx(-26.0).margin(0.1));
    CHECK(reading.momentary[0] == Approx(-26.0).margin(0.1));
}

TEST_CASE("doubling the signal adds 6 LU", "[engine][meter]") {
    OutputMeter quiet;
    feedSine(quiet, 48000, 2, {peakAmplitude(-40.0), peakAmplitude(-40.0)});
    OutputMeter loud;
    feedSine(loud, 48000, 2, {2.0 * peakAmplitude(-40.0), 2.0 * peakAmplitude(-40.0)});

    CHECK(loud.read().programme - quiet.read().programme == Approx(6.0206).margin(0.02));
}

TEST_CASE("channels past the second are neither metered nor summed", "[engine][meter]") {
    // 5.1, in WAVEFORMATEXTENSIBLE order: FL, FR, FC, LFE, BL, BR. The front pair is at -23 dBFS
    // and everything behind it is at full scale, and the meter must report the front pair -- this
    // build shows two bars, so it measures two channels, and a figure that folded in four
    // channels the user cannot see would be unreadable rather than more accurate. This is the
    // test that is supposed to fail when the panel grows a bar per channel.
    OutputMeter meter;
    feedSine(meter, 48000, 6, {peakAmplitude(-23.0), peakAmplitude(-23.0), 1.0, 1.0, 1.0, 1.0});

    const OutputMeter::Reading reading = meter.read();
    CHECK(reading.channels == 6);
    CHECK(OutputMeter::channelsShownFor(reading.channels) == 2);
    CHECK(reading.programme == Approx(-23.0).margin(0.1));
}

TEST_CASE("silence reads as silence rather than as very quiet", "[engine][meter]") {
    OutputMeter meter;
    feedSine(meter, 48000, 2, {0.0, 0.0});

    const OutputMeter::Reading reading = meter.read();
    CHECK(reading.programme == OutputMeter::kSilentLufs);
    CHECK(reading.momentary[0] == OutputMeter::kSilentLufs);
    CHECK(reading.peak[0] == 0.0f);
}

TEST_CASE("how many bars a stream shows follows the stream", "[engine][meter]") {
    // Nothing measured yet answers with all of them, because that is what the panel should draw
    // before the first block arrives; a mono stream corrects it to one.
    CHECK(OutputMeter::channelsShownFor(0) == OutputMeter::kMaxChannels);
    CHECK(OutputMeter::channelsShownFor(1) == 1);
    CHECK(OutputMeter::channelsShownFor(2) == OutputMeter::kMaxChannels);
    CHECK(OutputMeter::channelsShownFor(6) == OutputMeter::kMaxChannels);
}

TEST_CASE("the window has to fill before the reading is R128's", "[engine][meter]") {
    OutputMeter meter;

    // 20 hops of the 40 the window holds. There is a reading -- a meter that stayed blank for
    // 400 ms after every attach would look broken -- but it is not yet the standard's.
    feedSine(meter, 48000, 2, {peakAmplitude(-23.0), peakAmplitude(-23.0)}, 1000.0, 0.2);
    CHECK_FALSE(meter.read().settled);

    feedSine(meter, 48000, 2, {peakAmplitude(-23.0), peakAmplitude(-23.0)}, 1000.0, 0.5);
    const OutputMeter::Reading full = meter.read();
    CHECK(full.settled);
    CHECK(full.programme == Approx(-23.0).margin(0.1));
}

TEST_CASE("a sample rate change restarts the measurement", "[engine][meter]") {
    // A K-weighting filter is only correct for the rate it was designed at, and a window measured
    // across a format change describes neither format. The endpoint can change rate under us at
    // any time and announces it nowhere (sec. 4.5), so this is not a hypothetical.
    OutputMeter meter;
    feedSine(meter, 48000, 2, {peakAmplitude(-23.0), peakAmplitude(-23.0)});
    REQUIRE(meter.read().settled);

    feedSine(meter, 44100, 2, {peakAmplitude(-23.0), peakAmplitude(-23.0)}, 1000.0, 0.1);
    CHECK_FALSE(meter.read().settled);

    feedSine(meter, 44100, 2, {peakAmplitude(-23.0), peakAmplitude(-23.0)}, 1000.0, 0.6);
    const OutputMeter::Reading reading = meter.read();
    CHECK(reading.settled);
    CHECK(reading.programme == Approx(-23.0).margin(0.1));
}

TEST_CASE("the sample peak is the loudest since the last look, either sign", "[engine][meter]") {
    OutputMeter meter;

    Block quiet(2, 480);
    quiet.set(0, 0.25f);
    quiet.set(1, 0.25f);
    Block loud(2, 480);
    // Negative, because a peak meter that compared without taking the magnitude would report
    // nothing at all here -- and a chain that inverts phase is completely ordinary.
    loud.set(0, -0.8f);
    loud.set(1, 0.1f);

    protocol::PlanarView first = quiet.view();
    meter.measure(first, 48000);
    protocol::PlanarView second = loud.view();
    meter.measure(second, 48000);
    protocol::PlanarView third = quiet.view();
    meter.measure(third, 48000);

    const OutputMeter::Reading reading = meter.read();
    CHECK(reading.peak[0] == Approx(0.8f));
    CHECK(reading.peak[1] == Approx(0.25f));

    // Reset on read: the transient belongs to the look that saw it, and a meter that kept it
    // would latch at the loudest thing that ever happened.
    const OutputMeter::Reading after = meter.read();
    CHECK(after.peak[0] == 0.0f);
}

TEST_CASE("full scale is 0 dBFS and silence is not a number near it", "[engine][meter]") {
    CHECK(OutputMeter::toDbfs(1.0f) == Approx(0.0f).margin(1e-6));
    CHECK(OutputMeter::toDbfs(0.5f) == Approx(-6.0206f).margin(1e-4));
    CHECK(OutputMeter::toDbfs(0.0f) == OutputMeter::kSilentLufs);
    // Past full scale is reportable rather than clamped: the audio engine clips it on the way to
    // the device, and the meter is the only thing that can say so first.
    CHECK(OutputMeter::toDbfs(2.0f) == Approx(6.0206f).margin(1e-4));
}

TEST_CASE("metering reads the block and never writes it", "[engine][meter]") {
    // The conformance harness's round-trip fidelity claim depends on a passed-through block
    // coming back bit-for-bit, and the meter now sits on that path.
    engine::ChainProcessor processor;

    Block block(2, 64);
    block.set(0, 0.1234567f);
    block.set(1, -0.7654321f);

    ipc::BlockInfo info;
    info.sampleRate = 48000;
    info.channelCount = 2;
    info.audio = block.view();
    processor.processBlock(info);

    for (std::int32_t f = 0; f < block.frames(); ++f) {
        REQUIRE(block.at(0, f) == 0.1234567f);
        REQUIRE(block.at(1, f) == -0.7654321f);
    }
    const OutputMeter::Reading reading = processor.outputMeter().read();
    CHECK(reading.peak[1] == Approx(0.7654321f));
}

TEST_CASE("a bypassed block is metered, because it is still the output", "[engine][meter]") {
    engine::ChainProcessor processor;
    processor.setBypassed(true);

    Block block(2, 480);
    block.set(0, 0.5f);
    block.set(1, 0.5f);
    ipc::BlockInfo info;
    info.sampleRate = 48000;
    info.channelCount = 2;
    info.audio = block.view();
    processor.processBlock(info);

    REQUIRE(processor.blocksBypassed() == 1);
    CHECK(processor.outputMeter().read().peak[0] == Approx(0.5f));
}

TEST_CASE("metering costs the audio thread no violations", "[engine][meter][rt]") {
    if constexpr (!rt::checksEnabled()) {
        SUCCEED("violation detector compiled out");
        return;
    }

    OutputMeter meter;
    Block block(2, 480);
    block.set(0, 0.5f);
    block.set(1, -0.5f);

    rt::resetViolations();
    {
        // With the sec. 7.4.1 rules in force, and across a rate change, because that is the one
        // path in here that does arithmetic beyond the per-sample loop.
        const rt::RealtimeGuard guard;
        for (int pass = 0; pass < 8; ++pass) {
            protocol::PlanarView view = block.view();
            meter.measure(view, pass < 4 ? 48000 : 44100);
        }
    }
    const rt::ViolationCounts counts = rt::violations();
    INFO("allocations " << counts.allocations << " frees " << counts.deallocations << " locks " << counts.locks);
    CHECK(counts.total() == 0);
    rt::resetViolations();
}

TEST_CASE("a meter that is switched off measures nothing at all", "[engine][meter]") {
    OutputMeter meter;
    // On until something asks otherwise, so a test or anything else with no window attached
    // measures as it always did.
    REQUIRE(meter.enabled());

    meter.setEnabled(false);
    feedSine(meter, 48000, 2, {1.0, 1.0});

    const OutputMeter::Reading reading = meter.read();
    // Not merely "unchanged": the block counter has to stay where it was, because that is what
    // tells a reader that looks anyway that nothing is being measured rather than that the audio
    // has gone quiet.
    CHECK(reading.blocks == 0);
    CHECK_FALSE(reading.settled);
    CHECK(reading.programme == OutputMeter::kSilentLufs);
    CHECK(reading.peak[0] == 0.0f);
}

TEST_CASE("switching a meter back on restarts it rather than resuming it", "[engine][meter]") {
    OutputMeter meter;
    feedSine(meter, 48000, 2, {peakAmplitude(-23.0), peakAmplitude(-23.0)});
    REQUIRE(meter.read().settled);

    meter.setEnabled(false);
    feedSine(meter, 48000, 2, {1.0, 1.0});
    REQUIRE(meter.read().programme == Approx(-23.0).margin(0.1));

    meter.setEnabled(true);
    // One hop in. If the window had been resumed rather than restarted it would still be full,
    // and the first thing shown after a restore would be a blend of now and of whatever was
    // playing when the window was minimized -- read as though it were now.
    feedSine(meter, 48000, 2, {peakAmplitude(-35.0), peakAmplitude(-35.0)}, 1000.0, 0.05);
    CHECK_FALSE(meter.read().settled);

    feedSine(meter, 48000, 2, {peakAmplitude(-35.0), peakAmplitude(-35.0)}, 1000.0, 0.6);
    const OutputMeter::Reading reading = meter.read();
    CHECK(reading.settled);
    CHECK(reading.programme == Approx(-35.0).margin(0.1));
}
