// The output meter strip: what the machine is actually putting out, in LUFS.
//
// It sits between the rack list and the rack's buttons because that is where the output of the
// chain is (project owner, 2026-08-30). It is not part of the rack -- it goes on reading with
// nothing loaded, with every plugin bypassed and with the whole chain switched out of the path,
// because all of those still hand audio back to the endpoint.
//
// What it shows and why that is worth the pixels: every other display in this window proves
// blocks are *flowing*. A chain that outputs silence -- a plugin misconfigured, a bus wired to
// nothing, a plugin that muted itself on a preset change -- looks identical to a working one from
// the counters, and the log has nothing to say about it either. This is the only thing on screen
// that can tell those two apart, which is why it is next to the rack and not in a dialog.
//
// **The measurement is EBU R128 momentary loudness**, and it lives in `engine/output_meter.h`
// along with the reasons: K-weighted, 400 ms sliding window, one bar per channel, two bars at
// most. Nothing here filters anything; the audio thread has already done the work and this reads
// one struct off it. What this file owns is the *ballistics and the drawing* -- which is a real
// division, because the standard says what to measure and says nothing about how a meter should
// look while measuring it.
//
// Three decisions about that are worth naming.
//
// **No decay on the bar.** A peak meter needs a fall time invented for it; a loudness meter does
// not, because the 400 ms window *is* the ballistic. Adding a second one on top would make the
// meter lag the standard it claims to implement. The bar therefore shows the reading, and the
// only invented movement here is the peak-hold marker, which is the loudest momentary value of
// the last couple of seconds and is a display convenience with no standing in R128.
//
// **A stalled stream falls to silence rather than freezing.** A sliding window with nothing new
// going into it holds its last value forever, so after a detach the bar would sit at whatever was
// playing when the user pressed it. `Reading::blocks` is what makes that detectable, and the
// display then falls over the same 400 ms the window would have taken.
//
// **Nothing is measured or drawn while the window is off the screen** (project owner,
// 2026-08-30). Minimized, hidden, or sitting on a virtual desktop the user is not looking at: in
// all three the refresh stops *and* `OutputMeter` is switched off, so the K-weighting stops
// running on the audio thread as well. The repaint is the larger of the two costs by some
// distance -- thirty frames a second of nothing -- but the measurement is the one being paid on a
// thread with a deadline, and neither is worth paying for a picture nobody can see.
//
// It is a poll and not an event, and deliberately: a slow timer that asks "am I on screen" is one
// mechanism covering all three cases, where the event route needs a window-state filter for
// minimize, show and hide events for the third, and *still* a poll for the virtual desktop, which
// Qt does not report at all. The poll costs five cheap Win32 calls a second while idle and
// self-heals if an event is ever missed. Resuming inside 200 ms is under the 400 ms the window
// needs to refill anyway.
//
// What is *not* covered is a window that is on screen but entirely behind another one. There is
// no cheap or reliable way to ask Windows that -- the browsers that do it maintain a region tree
// off accessibility hooks -- and a wrong answer would blank a meter the user is looking at.
//
// **Loudness cannot see clipping, so something else has to.** A plugin that pushes past full
// scale is clipped by the audio engine on the way to the device and nothing says so. The sample
// peak from the same pass over the block drives a latch at the top of each bar, held long enough
// to be seen by somebody who was looking elsewhere.

#pragma once

#include "engine_host.h"

#include "aip/engine/output_meter.h"

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>

class QLabel;
class QTimer;

namespace aip::ui {

/// "-23.4", "-inf" -- one fader-width spelling of a loudness, in one place because the bar's
/// readout and anything else that wants to say the same number must not disagree in the digit.
[[nodiscard]] QString loudnessText(float lufs);

/// The bars themselves, and nothing else: no timer, no engine, no ballistics. Handed a state and
/// asked to draw it, which is what makes the drawing testable by eye in isolation and keeps the
/// paint code away from the thread and lifetime questions in MeterPanel.
class LevelMeter final : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeter(QWidget* parent = nullptr);

    struct State {
        std::size_t bars = engine::OutputMeter::kMaxChannels;
        /// LUFS, or `kSilentLufs`.
        float level[engine::OutputMeter::kMaxChannels] = {};
        /// The loudest recent level, as a marker. Same units.
        float hold[engine::OutputMeter::kMaxChannels] = {};
        /// A sample at or past full scale, recently enough to still be worth saying.
        bool over[engine::OutputMeter::kMaxChannels] = {};
    };

    void setState(const State& state);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// Where a loudness sits in the widget, in device pixels from the top. Clamped to the scale,
    /// so a reading past full scale draws at the top rather than off the widget.
    [[nodiscard]] int yFor(float lufs, int top, int height) const;

    State state_;
};

/// The strip: a label, the bars, and the programme figure under them. Owns the refresh and is the
/// meter's **one reader** -- `OutputMeter::read` clears the sample peak, so a second caller would
/// take transients this one then never sees.
class MeterPanel final : public QWidget {
    Q_OBJECT

public:
    explicit MeterPanel(EngineHost& host, QWidget* parent = nullptr);

private:
    /// Every tick, at whichever of the two rates is in force. Decides whether the meter is on
    /// screen, then refreshes only if it is.
    void tick();

    void refresh();

    /// Starts or stops the measurement and the fast refresh, when the answer has changed.
    void updateActivity();

    /// Whether anyone can currently see this. False when the widget is hidden, when its window is
    /// minimized, and when the window is cloaked -- which is what Windows does to a window on a
    /// virtual desktop other than the one in front.
    [[nodiscard]] bool onScreen();

    /// Puts the bars and the readout back to silence, so a restore does not flash whatever was
    /// playing when the window went away.
    void clearDisplay();

    /// Thirty frames a second while it is being watched; five times a second while it is not,
    /// which is only ever asking whether it is being watched again.
    static constexpr int kActiveIntervalMs = 33;
    static constexpr int kIdleIntervalMs = 200;

    /// How long a peak-hold marker stays where it was put, and how fast it falls afterwards.
    static constexpr double kHoldSeconds = 2.0;
    static constexpr double kHoldFallLuPerSecond = 12.0;

    /// How long an over indicator stays lit. Long, deliberately: the whole point of it is to be
    /// seen by somebody who was not looking at the moment it happened.
    static constexpr double kOverSeconds = 2.5;

    /// What a stalled stream costs the display, matching the window that is no longer sliding.
    static constexpr double kStallFallSeconds = 0.4;

    EngineHost& host_;
    LevelMeter* meter_ = nullptr;
    QLabel* readout_ = nullptr;
    QTimer* timer_ = nullptr;

    /// Whether the meter is currently measuring and refreshing. Starts false: a widget that has
    /// not been shown yet is not on screen, and the first tick corrects it.
    bool active_ = false;

    LevelMeter::State state_;
    double holdAge_[engine::OutputMeter::kMaxChannels] = {};
    double overAge_[engine::OutputMeter::kMaxChannels] = {};
    float programme_ = 0.0f;

    /// The block count at the last refresh. Equal to this one's means no audio arrived since.
    std::uint64_t blocks_ = 0;
    QElapsedTimer clock_;
};

} // namespace aip::ui
