// The process's own memory footprint -- the other half of the sec. 7.4.3 acceptance criterion.
//
// The counters already on screen prove that the audio thread allocates nothing. They say nothing
// about whether anything is leaking *off* it, and by construction they cannot: the detector is a
// per-image `operator new` replacement scoped to real-time sections, so every allocation on the
// control plane is invisible to it. A shell that has been up for a day, with editors opened and
// closed, plugins added and removed and a scan or two behind it, has plenty of places to leak that
// the violation counters would report as zero throughout.
//
// A flat resident set over a long soak is what closes that gap, and it is only evidence if someone
// can read it without attaching a profiler. Windows keeps both numbers already, so this is a read
// rather than an accounting exercise.

#pragma once

#include <cstddef>

namespace aip::ui {

/// What the process is holding, in bytes. Both zero when Windows declines to answer, which is not
/// worth distinguishing from "nothing yet" for a pair of numbers that exist only to be looked at.
struct ProcessFootprint {
    std::size_t residentBytes = 0;

    /// The high-water mark, maintained by the kernel since the process started. The one that
    /// matters in a soak: a leak that memory pressure has since trimmed out of the working set
    /// still shows here, and the current figure alone would have quietly forgotten it.
    ///
    /// Since the process started, not since a reset -- there is nothing in the shell to reset it
    /// with, and a soak wants it measured from the start regardless.
    std::size_t peakResidentBytes = 0;
};

/// Reads both figures. Cheap enough for the servicing tick; it is one kernel call and no
/// allocation.
[[nodiscard]] ProcessFootprint processFootprint();

} // namespace aip::ui
