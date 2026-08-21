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

inline void note(std::atomic<std::uint64_t>& counter) noexcept {
    if (tlsRealtimeDepth == 0) {
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
    detail::note(detail::gAllocations);
#endif
}

inline void noteDeallocation() noexcept {
#if defined(AIP_RT_CHECKS)
    detail::note(detail::gDeallocations);
#endif
}

inline void noteLock() noexcept {
#if defined(AIP_RT_CHECKS)
    detail::note(detail::gLocks);
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
