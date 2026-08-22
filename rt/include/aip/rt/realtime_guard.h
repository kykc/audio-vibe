// Real-time violation detector -- the substitute for clang's RealtimeSanitizer, which is
// unavailable under MSVC (design_doc.md sec. 7.4.6).
//
// A thread-local "inside real-time section" depth is raised by RealtimeGuard at the top of the
// valet callback. Global `operator new`/`operator delete` overrides (rt/src/alloc_hooks.cpp)
// and the assert-only locking wrappers below check it and bump a counter when they are reached
// from inside such a section.
//
// Counting, not aborting, is the primary mechanism: sec. 7.4.3 makes "steady state performs
// exactly zero audio-thread allocations" a directly testable acceptance criterion, and the
// soak test asserts on these counters. `setBreakOnViolation(true)` additionally traps into the
// debugger at the offending call site, which is what you want in an interactive session.
//
// Compiled into RelWithDebInfo (AIP_RT_CHECKS), compiled out of Release: with checks off every
// entry point below is an empty inline function and RealtimeGuard is an empty object.

#pragma once

#include <atomic>
#include <cstdint>

#if defined(AIP_RT_CHECKS) && defined(_MSC_VER)
#include <intrin.h>
#endif

namespace aip::rt {

/// The three violation classes the detector can observe. Everything else in sec. 7.4.1 (I/O, the
/// loader, COM, exceptions) is caught by code review, not by this counter.
struct ViolationCounts {
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t locks = 0;

    [[nodiscard]] std::uint64_t total() const noexcept {
        return allocations + deallocations + locks;
    }

    [[nodiscard]] friend bool operator==(const ViolationCounts&,
                                         const ViolationCounts&) = default;
};

namespace detail {

#if defined(AIP_RT_CHECKS)
inline thread_local unsigned tlsRealtimeDepth = 0;
inline std::atomic<std::uint64_t> gAllocations{0};
inline std::atomic<std::uint64_t> gDeallocations{0};
inline std::atomic<std::uint64_t> gLocks{0};
inline std::atomic<bool> gBreakOnViolation{false};
/// Non-null while a ViolationProbe is in scope on this thread. See the class below.
inline thread_local ViolationCounts* tlsProbe = nullptr;

inline void note(std::atomic<std::uint64_t>& counter,
                 std::uint64_t ViolationCounts::*field) noexcept {
    if (tlsRealtimeDepth == 0) {
        return;
    }
    // A probe means the caller is deliberately exercising code that is *expected* to misbehave,
    // and wants to be told what it did rather than to have it charged against the process-wide
    // counters. Not breaking into the debugger either: a violation that was asked for is not a
    // bug, and trapping on it would make the probe unusable in an interactive session.
    if (tlsProbe != nullptr) {
        ++(tlsProbe->*field);
        return;
    }
    counter.fetch_add(1, std::memory_order_relaxed);
#if defined(_MSC_VER)
    if (gBreakOnViolation.load(std::memory_order_relaxed)) {
        __debugbreak();
    }
#endif
}
#endif

} // namespace detail

/// True when the calling thread is currently inside a real-time section.
[[nodiscard]] inline bool inRealtimeSection() noexcept {
#if defined(AIP_RT_CHECKS)
    return detail::tlsRealtimeDepth != 0;
#else
    return false;
#endif
}

inline void noteAllocation() noexcept {
#if defined(AIP_RT_CHECKS)
    detail::note(detail::gAllocations, &ViolationCounts::allocations);
#endif
}

inline void noteDeallocation() noexcept {
#if defined(AIP_RT_CHECKS)
    detail::note(detail::gDeallocations, &ViolationCounts::deallocations);
#endif
}

inline void noteLock() noexcept {
#if defined(AIP_RT_CHECKS)
    detail::note(detail::gLocks, &ViolationCounts::locks);
#endif
}

/// Snapshot of the process-wide violation counters. Cheap; safe to call from anywhere.
[[nodiscard]] inline ViolationCounts violations() noexcept {
#if defined(AIP_RT_CHECKS)
    return ViolationCounts{detail::gAllocations.load(std::memory_order_relaxed),
                           detail::gDeallocations.load(std::memory_order_relaxed),
                           detail::gLocks.load(std::memory_order_relaxed)};
#else
    return ViolationCounts{};
#endif
}

inline void resetViolations() noexcept {
#if defined(AIP_RT_CHECKS)
    detail::gAllocations.store(0, std::memory_order_relaxed);
    detail::gDeallocations.store(0, std::memory_order_relaxed);
    detail::gLocks.store(0, std::memory_order_relaxed);
#endif
}

/// When enabled, a violation traps into the debugger at the offending call site.
inline void setBreakOnViolation(bool enabled) noexcept {
#if defined(AIP_RT_CHECKS)
    detail::gBreakOnViolation.store(enabled, std::memory_order_relaxed);
#else
    (void)enabled;
#endif
}

/// True when the detector is compiled in. Tests that assert on counters must skip without it.
[[nodiscard]] inline constexpr bool checksEnabled() noexcept {
#if defined(AIP_RT_CHECKS)
    return true;
#else
    return false;
#endif
}

/// Runs a scope *as if* it were an audio-thread callback, and counts what it did privately.
///
/// This exists for one job: exercising third-party code on the control thread on purpose, to make
/// it do its first-call misbehaviour somewhere harmless. `PluginInstance::warmUp` runs a few
/// blocks through a plugin the moment it is prepared, so that a plugin which allocates on its
/// first `process` -- a great many do -- allocates *there*, on the control thread, rather than on
/// the valet thread the first time the user plays something.
///
/// A plain RealtimeGuard would be wrong for that. Its counters are process-wide, and sec. 7.4.3's
/// acceptance criterion is that they are *exactly zero* after steady state; charging a deliberate
/// warm-up against them would make the one number this project is most careful about mean
/// something else. So the probe diverts counting into itself for the duration of the scope, and
/// the globals never see it.
///
/// Nests: an inner probe takes over and the outer one resumes when it leaves. Not thread-safe and
/// does not need to be -- the diversion is thread-local, like the depth it rides on.
class ViolationProbe {
public:
    ViolationProbe() noexcept {
#if defined(AIP_RT_CHECKS)
        ++detail::tlsRealtimeDepth;
        previous_ = detail::tlsProbe;
        detail::tlsProbe = &counts_;
#endif
    }

    ~ViolationProbe() {
#if defined(AIP_RT_CHECKS)
        detail::tlsProbe = previous_;
        --detail::tlsRealtimeDepth;
#endif
    }

    ViolationProbe(const ViolationProbe&) = delete;
    ViolationProbe& operator=(const ViolationProbe&) = delete;

    /// What the scope has done so far. Zero in a build without the detector, which is why
    /// callers must report it rather than assert on it.
    [[nodiscard]] ViolationCounts counts() const noexcept { return counts_; }

private:
    ViolationCounts counts_{};
    [[maybe_unused]] ViolationCounts* previous_ = nullptr;
};

/// RAII marker for a real-time section. Place one at the top of every audio-thread callback.
/// Nesting is supported; the guard is reentrant per thread.
class RealtimeGuard {
public:
    RealtimeGuard() noexcept {
#if defined(AIP_RT_CHECKS)
        ++detail::tlsRealtimeDepth;
#endif
    }

    ~RealtimeGuard() noexcept {
#if defined(AIP_RT_CHECKS)
        --detail::tlsRealtimeDepth;
#endif
    }

    RealtimeGuard(const RealtimeGuard&) = delete;
    RealtimeGuard& operator=(const RealtimeGuard&) = delete;
};

/// Temporarily leaves the real-time section. Only for code that provably runs on the control
/// thread but shares a call path with the audio thread -- needing this is usually a smell.
class RealtimeEscape {
public:
    RealtimeEscape() noexcept {
#if defined(AIP_RT_CHECKS)
        saved_ = detail::tlsRealtimeDepth;
        detail::tlsRealtimeDepth = 0;
#endif
    }

    ~RealtimeEscape() noexcept {
#if defined(AIP_RT_CHECKS)
        detail::tlsRealtimeDepth = saved_;
#endif
    }

    RealtimeEscape(const RealtimeEscape&) = delete;
    RealtimeEscape& operator=(const RealtimeEscape&) = delete;

private:
#if defined(AIP_RT_CHECKS)
    unsigned saved_ = 0;
#endif
};

} // namespace aip::rt
