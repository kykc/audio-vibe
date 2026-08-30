#include "level_meter.h"

#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <windows.h>

#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace aip::ui {
namespace {

using engine::OutputMeter;

/// The scale's ends, and the marks on it. -23 is EBU R128's programme target and is the one line
/// here that means something outside this application, so it is drawn brighter than the rest.
constexpr float kFloor = OutputMeter::kFloorLufs;
constexpr float kTop = OutputMeter::kFullScaleLufs;
constexpr float kTicks[] = {-6.0f, -12.0f, -18.0f, -23.0f, -30.0f, -40.0f, -50.0f};

/// Where the bar changes colour. Deliberately *not* R128's -23 target, which is a broadcast
/// delivery figure and not a warning: consumer audio sits around -14 LUFS and a mastered-loud
/// record nearer -8, so a meter that turned amber at -23 would be amber all day and would be
/// saying nothing. What these say instead is headroom -- loudness this high on programme material
/// means peaks near full scale, which is the thing that actually goes wrong here. The -23 mark is
/// still drawn on the scale, as the reference it is.
constexpr float kWarnLufs = -14.0f;
constexpr float kHotLufs = -6.0f;

/// Fixed rather than taken from the palette, because these three carry meaning -- quiet, loud,
/// nearly out of headroom -- and a palette that recoloured them would be recolouring the reading.
/// Chosen to hold their contrast on both a light and a dark background.
const QColor kCalm(72, 176, 104);
const QColor kWarn(214, 172, 72);
const QColor kHot(206, 92, 76);
const QColor kOver(232, 72, 60);

QColor colourFor(float lufs) {
    if (lufs >= kHotLufs) {
        return kHot;
    }
    return lufs >= kWarnLufs ? kWarn : kCalm;
}

} // namespace

QString loudnessText(float lufs) {
    if (!(lufs > kFloor)) {
        // Below the bottom of the scale is not a number worth printing: a meter has to be able to
        // say "nothing" as distinct from "very quiet", and -73.4 says neither.
        return QStringLiteral("-inf");
    }
    return QString::number(lufs, 'f', 1);
}

LevelMeter::LevelMeter(QWidget* parent) : QWidget(parent) {
    for (std::size_t c = 0; c < OutputMeter::kMaxChannels; ++c) {
        state_.level[c] = OutputMeter::kSilentLufs;
        state_.hold[c] = OutputMeter::kSilentLufs;
    }
}

QSize LevelMeter::minimumSizeHint() const { return QSize(28, 60); }

QSize LevelMeter::sizeHint() const { return QSize(34, 180); }

void LevelMeter::setState(const State& state) {
    state_ = state;
    update();
}

int LevelMeter::yFor(float lufs, int top, int height) const {
    const float clamped = std::clamp(lufs, kFloor, kTop);
    const float fraction = (clamped - kFloor) / (kTop - kFloor);
    return top + static_cast<int>(std::lround((1.0f - fraction) * height));
}

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int bars = static_cast<int>(std::max<std::size_t>(1, state_.bars));
    const int spacing = 3;
    // Two pixels of margin top and bottom so the topmost tick and the over cap are not against
    // the edge of the widget.
    const int top = 2;
    const int barHeight = std::max(8, height() - 2 * top);
    const int barWidth = std::max(4, (width() - spacing * (bars - 1)) / bars);

    const QColor track = palette().color(QPalette::Base);
    const QColor tick = palette().color(QPalette::Mid);
    const QColor target = palette().color(QPalette::Text);

    for (int b = 0; b < bars; ++b) {
        const int x = b * (barWidth + spacing);
        const QRect rect(x, top, barWidth, barHeight);
        painter.fillRect(rect, track);

        // The scale, behind the reading. Faint, except R128's target, which is the one mark a
        // person can navigate by.
        for (const float mark : kTicks) {
            const int y = yFor(mark, top, barHeight);
            const bool isTarget = mark == OutputMeter::kTargetLufs;
            painter.setPen(isTarget ? target : tick);
            painter.setOpacity(isTarget ? 0.55 : 0.30);
            painter.drawLine(x, y, x + barWidth - 1, y);
        }
        painter.setOpacity(1.0);

        const float level = state_.level[static_cast<std::size_t>(b)];
        if (level > kFloor) {
            // Drawn in the three zones rather than in one colour picked from the top of the bar,
            // so the quiet part of a loud reading stays the colour it would be on its own -- which
            // is what makes the boundary between them readable as a scale mark in its own right.
            const struct {
                float from;
                float to;
                QColor colour;
            } zones[] = {{kFloor, kWarnLufs, kCalm}, {kWarnLufs, kHotLufs, kWarn}, {kHotLufs, kTop, kHot}};

            for (const auto& zone : zones) {
                if (level <= zone.from) {
                    continue;
                }
                const int zoneBottom = yFor(zone.from, top, barHeight);
                const int zoneTop = yFor(std::min(level, zone.to), top, barHeight);
                if (zoneBottom > zoneTop) {
                    painter.fillRect(QRect(x, zoneTop, barWidth, zoneBottom - zoneTop), zone.colour);
                }
            }
        }

        // The loudest of the last couple of seconds, as a line rather than as more fill: it is a
        // memory of the reading and must not be mistaken for the reading.
        const float hold = state_.hold[static_cast<std::size_t>(b)];
        if (hold > kFloor) {
            painter.fillRect(QRect(x, yFor(hold, top, barHeight) - 1, barWidth, 2), colourFor(hold));
        }

        // Full scale reached. Not part of the loudness scale at all -- it comes from the sample
        // peak -- so it is drawn as a cap across the top of the track rather than as part of the
        // bar, which is also where a person looks for it.
        if (state_.over[static_cast<std::size_t>(b)]) {
            painter.fillRect(QRect(x, top, barWidth, 3), kOver);
        }
    }
}

MeterPanel::MeterPanel(EngineHost& host, QWidget* parent) : QWidget(parent), host_(host) {
    // Said on the panel rather than only in the tooltip. A strip of bars between a plugin list and
    // a column of buttons could as easily be a per-plugin level, and this is the one label that
    // rules that out -- and the unit belongs on screen, because "LUFS" and "dBFS" are different
    // numbers for the same audio and the difference is the entire point of the change.
    auto* title = new QLabel(QStringLiteral("Out LUFS"), this);
    title->setAlignment(Qt::AlignHCenter);
    QFont compact = title->font();
    compact.setPointSizeF(std::max(6.0, compact.pointSizeF() - 1.0));
    title->setFont(compact);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 0, 4, 0);
    outer->setSpacing(2);
    outer->addWidget(title);

    meter_ = new LevelMeter(this);
    outer->addWidget(meter_, 1);

    readout_ = new QLabel(this);
    readout_->setFont(compact);
    readout_->setAlignment(Qt::AlignHCenter);
    // Sized for the widest reading it can hold, so the strip does not change width as the number
    // moves -- with the rack list beside it, that would move the list too.
    readout_->setMinimumWidth(QFontMetrics(compact).horizontalAdvance(QStringLiteral("-60.0")));
    outer->addWidget(readout_);

    setToolTip(
        QStringLiteral("The loudness of what this application is handing back to the endpoint: EBU R128 "
                       "momentary, K-weighted over a 400 ms window.\n\n"
                       "One bar per channel -- one for a mono endpoint, two otherwise; on an endpoint with more "
                       "than two channels these are the first two. The number underneath is the two together, "
                       "which is the figure BS.1770 defines.\n\n"
                       "The brighter line across the scale is R128's -23 LUFS target. The line inside a bar is "
                       "the loudest of the last couple of seconds. A red cap means a sample reached full scale "
                       "-- loudness cannot see that, and the audio engine clips it on the way to the device.\n\n"
                       "It reads the output whatever is happening to it, including while the chain is bypassed."));

    for (std::size_t c = 0; c < OutputMeter::kMaxChannels; ++c) {
        state_.level[c] = OutputMeter::kSilentLufs;
        state_.hold[c] = OutputMeter::kSilentLufs;
    }
    programme_ = OutputMeter::kSilentLufs;

    // Its own timer rather than EngineHost's 100 ms servicing tick. The window slides in 10 ms
    // hops, so a meter read ten times a second shows one value in three and stutters; this is the
    // one thing in the shell whose whole job is to move.
    //
    // It starts at the idle rate. Nothing has been shown yet, so nothing is on screen yet, and the
    // first tick after `show()` is what starts the measurement.
    timer_ = new QTimer(this);
    timer_->setInterval(kIdleIntervalMs);
    connect(timer_, &QTimer::timeout, this, &MeterPanel::tick);
    timer_->start();

    // Off until something can see it. `OutputMeter` is on by default, which is right for a test
    // or anything else with no window attached; this is the window attaching.
    host_.engine().outputMeter().setEnabled(false);

    clock_.start();
    clearDisplay();
}

bool MeterPanel::onScreen() {
    if (!isVisible()) {
        return false;
    }
    QWidget* top = window();
    if (top == nullptr || top->isMinimized()) {
        return false;
    }

    // The one state Qt has no word for: Windows cloaks a window that lives on a virtual desktop
    // other than the one in front, and reports it visible and un-minimized throughout. `winId()`
    // is safe to ask for here and only here -- the widget is shown, so its top level is already
    // native and this is not forcing a handle into existence early.
    const auto handle = reinterpret_cast<HWND>(top->winId());
    DWORD cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked == 0;
    }
    // No answer is not the same as "cloaked". A meter blanked by a failed query would be a defect
    // nobody could explain, where an extra meter refresh costs a few microseconds.
    return true;
}

void MeterPanel::updateActivity() {
    const bool visible = onScreen();
    if (visible == active_) {
        return;
    }
    active_ = visible;

    timer_->setInterval(active_ ? kActiveIntervalMs : kIdleIntervalMs);
    // The measurement and the refresh are switched together, always: a meter that measured
    // without drawing would be paying the audio thread for nothing, and one that drew without
    // measuring would be drawing a stale window.
    host_.engine().outputMeter().setEnabled(active_);
    clearDisplay();
}

void MeterPanel::clearDisplay() {
    LevelMeter::State cleared;
    for (std::size_t c = 0; c < OutputMeter::kMaxChannels; ++c) {
        state_.level[c] = OutputMeter::kSilentLufs;
        state_.hold[c] = OutputMeter::kSilentLufs;
        state_.over[c] = false;
        holdAge_[c] = 0.0;
        overAge_[c] = 0.0;
        cleared.level[c] = OutputMeter::kSilentLufs;
        cleared.hold[c] = OutputMeter::kSilentLufs;
    }
    cleared.bars = state_.bars;
    programme_ = OutputMeter::kSilentLufs;
    readout_->setText(loudnessText(programme_));
    meter_->setState(cleared);
}

void MeterPanel::tick() {
    updateActivity();
    if (!active_) {
        // Nothing to read and nothing to draw. The clock is restarted anyway, so the first active
        // tick after a restore measures its elapsed time from now rather than from whenever the
        // window was minimized.
        clock_.restart();
        return;
    }
    refresh();
}

void MeterPanel::refresh() {
    // The seconds since the last refresh, asked for rather than assumed: a timer set to 33 ms is
    // not a promise, and every fall rate below is per second.
    const double elapsed = std::min(0.25, static_cast<double>(clock_.restart()) / 1000.0);

    const OutputMeter::Reading reading = host_.engine().outputMeter().read();
    const bool stalled = reading.blocks == blocks_;
    blocks_ = reading.blocks;

    LevelMeter::State next;
    next.bars = OutputMeter::channelsShownFor(reading.channels);

    for (std::size_t c = 0; c < OutputMeter::kMaxChannels; ++c) {
        if (stalled) {
            // Nothing arrived. The window is not sliding, so its value is not falling either --
            // fall the display instead, over the same 400 ms it would have taken, so that a
            // detach leaves the meter at rest rather than frozen mid-programme.
            const auto fall = static_cast<float>((kTop - kFloor) * elapsed / kStallFallSeconds);
            state_.level[c] = state_.level[c] > kFloor ? state_.level[c] - fall : OutputMeter::kSilentLufs;
        } else {
            // No invented ballistic: the 400 ms window is the ballistic, and a second one on top
            // would make this lag the standard it implements.
            state_.level[c] = reading.momentary[c];
        }
        next.level[c] = state_.level[c];

        if (state_.level[c] >= state_.hold[c]) {
            state_.hold[c] = state_.level[c];
            holdAge_[c] = 0.0;
        } else {
            holdAge_[c] += elapsed;
            if (holdAge_[c] > kHoldSeconds) {
                state_.hold[c] =
                    std::max(state_.level[c], state_.hold[c] - static_cast<float>(kHoldFallLuPerSecond * elapsed));
            }
        }
        next.hold[c] = state_.hold[c];

        // At or past full scale. Latched for long enough to be seen, because the sample that did
        // it was one block long and nobody was looking at that moment.
        if (reading.peak[c] >= 1.0f) {
            state_.over[c] = true;
            overAge_[c] = 0.0;
        } else if (state_.over[c]) {
            overAge_[c] += elapsed;
            if (overAge_[c] > kOverSeconds) {
                state_.over[c] = false;
            }
        }
        next.over[c] = state_.over[c];
    }

    // The programme figure follows the bars: measured while audio is arriving, falling with them
    // when it stops, so the number and the picture never disagree.
    programme_ = stalled ? *std::max_element(state_.level, state_.level + next.bars) : reading.programme;
    const QString text = loudnessText(programme_);
    if (readout_->text() != text) {
        readout_->setText(text);
    }

    meter_->setState(next);
}

} // namespace aip::ui
