// The output loudness meter -- EBU R128 momentary, measured on the audio thread (sec. 7.4.1).
//
// What the valet hands back to the king, measured on the way out. This is the one display the
// shell has that says the system is doing what the user thinks it is: the counters prove blocks
// are *flowing*, and nothing else proves there is any audio in them. A chain that outputs silence
// -- a plugin misconfigured, a bus wired to nothing, a gain at the bottom -- looks exactly like a
// working one from every other number in the window.
//
// It is measured after the chain and on every path out of dispatch, including the bypassed one
// and the one where no chain is published: all three are the endpoint's audio being handed back,
// and the level of it is what the meter is for.
//
// **What is measured is loudness, not amplitude** (project owner, 2026-08-30). ITU-R BS.1770-4 /
// EBU R128:
//
//   1. K-weighting -- a shelving pre-filter and an RLB high-pass, per channel
//   2. mean square of the weighted signal over a sliding window
//   3. loudness = -0.691 + 10 log10 (sum over channels of G_i * z_i), with G = 1 for L and R
//
// The window is **400 ms**, which is R128's *momentary* loudness (EBU Tech 3341 M) and the one
// intended for live metering -- short-term is 3 s and integrated is gated over a whole programme,
// and neither moves fast enough to watch. The window and the refresh rate are separate things:
// Tech 3341 computes M every 100 ms, which is a measurement cadence and not a display one, so
// this slides the same 400 ms window in 10 ms hops and lets the widget read it as it repaints.
//
// The coefficients are derived from the standard's analog prototype rather than pasted, so every
// sample rate is handled and 48 kHz reproduces the ITU table to every digit it publishes -- there
// is a test that asserts exactly that, and another that puts EBU Tech 3341's first test signal
// through this class and requires -23.0 LUFS out of it.
//
// **Two channels, and how they map.** Which channels are shown follows the stream: one bar for a
// mono endpoint, two for anything wider, and on an endpoint with more than two channels the bars
// are channels 0 and 1 with the rest neither measured nor summed (project owner, 2026-08-30). So
// on 5.1 the programme figure is the front pair's loudness and not the programme's -- that is the
// honest reading of a two-bar meter, and `kMaxChannels` is the only number to change when the
// panel grows a bar per channel.
//
// **Real-time safety.** `measure` is the only member the audio thread calls. Fixed storage sized
// at construction, one pass over at most two channels, four multiply-accumulates per sample, and
// a handful of relaxed atomic stores per block: no allocation, no lock, no unbounded loop
// (sec. 7.4.1). The one arithmetic-heavy path is recomputing the filter for a new sample rate,
// which is a few calls to `tan` and `pow` -- no heap and no lock, so it is permitted, and it
// happens once per format change, which is already a transition sec. 7.4.3 allows to be audible.
// The logarithms are all on the reader's side.

#pragma once

#include "aip/protocol/planar.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aip::engine {

class OutputMeter {
public:
    OutputMeter() noexcept;

    OutputMeter(const OutputMeter&) = delete;
    OutputMeter& operator=(const OutputMeter&) = delete;

    /// Channels this build meters and displays. See the note above before changing it.
    static constexpr std::size_t kMaxChannels = 2;

    /// R128 momentary: 40 hops of 10 ms. The hop is the resolution the window slides at, and is
    /// what lets a widget repainting at 30 Hz see a different value every time.
    static constexpr std::size_t kSubBlocks = 40;
    static constexpr std::size_t kHopMs = 10;
    static constexpr std::size_t kWindowMs = kSubBlocks * kHopMs;

    /// The bottom of the scale a meter drawn from this should use, and the top. Not enforced here
    /// -- `read` reports what it measured -- but they are the one place the numbers are written
    /// down, so the widget and any test agree on where silence and full scale are.
    static constexpr float kFloorLufs = -60.0f;
    static constexpr float kFullScaleLufs = 0.0f;

    /// EBU R128's programme target, and the one number on this scale that means something to
    /// somebody. The widget marks it.
    static constexpr float kTargetLufs = -23.0f;

    /// What silence reads. Negative infinity rather than a very large negative number: a meter
    /// has to be able to tell "nothing at all" from "very quiet", and so does anything turning
    /// this into a label.
    static constexpr float kSilentLufs = -std::numeric_limits<float>::infinity();

    // --------------------------------------------------------------- switched off ------------
    //
    // Either thread. A meter nobody is looking at is work nobody wants done, and this one is done
    // on the audio thread -- so when the window is minimized, hidden, or on a virtual desktop the
    // user is not on, the shell switches it off and `measure` returns on its first line
    // (level_meter.h decides when). Two biquads per sample over two channels is not a large cost
    // beside a plugin chain, but it is a cost paid on the one thread in this system that has a
    // deadline, for a picture on a window that is not on the screen.
    //
    // Switching it back on **restarts the measurement** rather than resuming it. The window would
    // otherwise hold hops from before the window was minimized and the filter would hold state
    // from a signal that may be minutes old, and the first 400 ms after a restore would be a blend
    // of the two, read as though it were now. The restart is done by the audio thread on its next
    // block, because that is the thread that owns the window and the filter state.
    //
    // Nothing turns it off by itself: it is on until something asks otherwise, so a meter with no
    // UI attached -- a test, a future headless client -- measures as it always did.

    void setEnabled(bool enabled) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // -------------------------------------------------------------------- audio thread -------

    /// Folds one block into the sliding window. Returns immediately when the meter is switched
    /// off, before anything is measured and before `blocks` is advanced -- a reader that looks
    /// anyway sees a stalled stream, which is the truth: nothing is being measured. `sampleRate` comes from the block
    /// header on every block and is never cached across one (sec. 4.5): a rate change here rebuilds the filter and
    /// restarts the window, because a K-weighting filter is only correct for the rate it was
    /// designed at. A change of channel count does the same, so that a stale channel from a
    /// previous stream cannot go on being displayed.
    ///
    /// Does nothing for a block with no channels, no frames, or no sample rate -- which is what a
    /// malformed header reduces to.
    void measure(const protocol::PlanarView& audio, std::uint32_t sampleRate) noexcept;

    // ------------------------------------------------------------------ control thread -------

    /// One coherent look at the meter.
    struct Reading {
        /// Per-channel momentary loudness, LUFS: the BS.1770 formula over one channel with a
        /// weight of 1. Silence reads `kSilentLufs`. Only the first
        /// `channelsShownFor(channels)` entries mean anything.
        float momentary[kMaxChannels] = {kSilentLufs, kSilentLufs};
        /// The same over the shown channels together, which is the figure BS.1770 actually
        /// defines and the one worth putting a number on screen for. On a stream wider than two
        /// channels it is the front pair's, not the programme's -- see the note above.
        float programme = kSilentLufs;
        /// Sample peak since the last read, linear and absolute: 1.0 is full scale. Loudness
        /// cannot say anything about clipping, and something has to -- a plugin that pushes past
        /// full scale is clipped by the audio engine on the way to the device, silently.
        float peak[kMaxChannels] = {};
        /// The stream's own width, as the last measured block reported it. Zero until one has
        /// been. It is the *stream's*, not the number of bars.
        std::uint32_t channels = 0;
        /// Blocks measured since this object was created. A reader that sees it unchanged since
        /// its last look knows no audio arrived in between, which is the one thing the loudness
        /// figure cannot say for itself: a sliding window with nothing new going into it holds
        /// its last value forever, and a meter frozen at -14 LUFS after a detach is worse than no
        /// meter at all.
        std::uint64_t blocks = 0;
        /// False until a whole 400 ms window has been measured. The loudness is reported before
        /// that from what there is, so the meter says something immediately after an attach
        /// rather than staying blank; it is only fully R128's number once this is true.
        bool settled = false;
    };

    /// **One reader only.** The loudness figures are a plain look at a sliding measure and could
    /// be read by anyone, but the sample peak is peak-since-you-last-looked and is cleared here;
    /// two readers would each see part of it and neither would see the transient.
    [[nodiscard]] Reading read() noexcept;

    /// How many bars a stream of `channelCount` channels shows. Zero -- "nothing measured yet" --
    /// answers with all of them, because that is what the panel should draw before the first block
    /// arrives; a mono stream corrects it to one.
    [[nodiscard]] static std::size_t channelsShownFor(std::uint32_t channelCount) noexcept;

    /// Linear peak to dBFS: exactly 0 at full scale, `kSilentLufs` at silence.
    [[nodiscard]] static float toDbfs(float peak) noexcept;

private:
    /// One direct-form-II transposed biquad. Double throughout, because the RLB section has its
    /// poles at 38 Hz and a float state at 192 kHz loses the low end of the very thing it is
    /// there to remove.
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

        [[nodiscard]] double process(double x, double (&state)[2]) const noexcept {
            const double y = b0 * x + state[0];
            state[0] = b1 * x - a1 * y + state[1];
            state[1] = b2 * x - a2 * y;
            return y;
        }
    };

    /// Audio thread. Designs the K-weighting filter for `sampleRate` and throws the window away.
    void reset(std::uint32_t sampleRate, std::uint32_t channelCount) noexcept;

    /// Audio thread. Closes the 10 ms hop being filled and slides the window on to the next.
    void closeHop() noexcept;

    // --------------------------------------------------------- audio thread state only -------

    Biquad pre_;
    Biquad rlb_;
    double preState_[kMaxChannels][2] = {};
    double rlbState_[kMaxChannels][2] = {};

    /// The 400 ms window: `kSubBlocks` completed hops per channel, plus the one being filled.
    double hops_[kMaxChannels][kSubBlocks] = {};
    double partial_[kMaxChannels] = {};
    std::size_t hopIndex_ = 0;
    std::size_t hopsFilled_ = 0;
    std::int32_t hopSamples_ = 0;
    std::int32_t hopFilledSamples_ = 0;

    std::uint32_t preparedRate_ = 0;
    std::uint32_t preparedChannels_ = 0;

    // -------------------------------------------------------------------- published ----------

    /// Mean square of the K-weighted signal over the window, per channel. Linear; the reader
    /// takes the logarithm. Relaxed on both sides: it orders nothing and guards nothing.
    std::atomic<float> meanSquare_[kMaxChannels];

    /// Sample peak since the last read. The audio thread loads, compares and stores rather than
    /// looping on a compare-exchange, and the race that allows is worth stating because it is why
    /// no loop is needed: a read landing between the load and the store can cost one block's
    /// peak, but only when that block's peak was *lower* than the value the reader just took --
    /// which it has therefore already seen. A transient can never be lost this way.
    std::atomic<float> peak_[kMaxChannels];

    std::atomic<std::uint32_t> channels_{0};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<bool> settled_{false};

    /// See setEnabled(). Relaxed on both sides: nothing is ordered against it, and the worst a
    /// stale read costs is one block measured either side of a window being minimized.
    std::atomic<bool> enabled_{true};
    /// Raised by `setEnabled(true)` and lowered by the audio thread when it has acted on it.
    /// The restart cannot happen where it is asked for, because the window and the filter state
    /// belong to the audio thread.
    std::atomic<bool> restart_{false};
};

} // namespace aip::engine
