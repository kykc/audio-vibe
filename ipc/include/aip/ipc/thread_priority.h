// Audio thread promotion, exactly as the reference implementation does it (design_doc.md sec. 4.6).
//
// This is behavioural rather than wire-level, but it is required for glitch-free operation: the
// king blocks the audio engine's own real-time thread waiting for us (sec. 3.7.1), so the valet
// thread must not be preempted by ordinary work.
//
// A thread at AVRT_PRIORITY_CRITICAL will starve normal-priority threads if it blocks on
// anything they hold. That is what makes the sec. 7.4 real-time rules a correctness requirement
// rather than a performance preference.

#pragma once

#include <windows.h>

#include <avrt.h>

namespace aip::ipc {

/// Promotes the calling thread for the lifetime of the object and reverts on destruction.
/// Construct it on the audio thread itself, at the top of its entry point.
class ProAudioPriority {
public:
    ProAudioPriority() noexcept;
    ~ProAudioPriority();

    ProAudioPriority(const ProAudioPriority&) = delete;
    ProAudioPriority& operator=(const ProAudioPriority&) = delete;

    /// True when `AvSetMmThreadCharacteristics` succeeded. A false value is not fatal -- the
    /// thread still runs at priority 15 -- but it is worth surfacing, since it means the MMCSS
    /// guarantee is absent.
    [[nodiscard]] bool mmcssActive() const noexcept { return task_ != nullptr; }

    /// True when `SetThreadPriority` succeeded.
    [[nodiscard]] bool priorityRaised() const noexcept { return priorityRaised_; }

private:
    HANDLE task_ = nullptr;
    bool priorityRaised_ = false;
};

} // namespace aip::ipc
