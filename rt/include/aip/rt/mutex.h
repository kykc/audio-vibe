// Assert-only wrappers around the locking primitives we might accidentally reach for
// (design_doc.md sec. 7.4.6, item 1). Use `aip::rt::Mutex` instead of `std::mutex` everywhere in
// this codebase: it behaves identically, but taking it from inside a real-time section is
// counted as a violation instead of silently inverting priority against `audiodg.exe`.
//
// Locks are forbidden on the audio thread with no exceptions (sec. 7.4.1). The only sanctioned
// blocking calls there are the KING/VALET event operations (sec. 7.4.4).

#pragma once

#include "aip/rt/realtime_guard.h"

#include <mutex>

namespace aip::rt {

class Mutex {
public:
    Mutex() = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() {
        noteLock();
        impl_.lock();
    }

    bool try_lock() {
        noteLock();
        return impl_.try_lock();
    }

    void unlock() { impl_.unlock(); }

private:
    std::mutex impl_;
};

using ScopedLock = std::lock_guard<Mutex>;
using UniqueLock = std::unique_lock<Mutex>;

} // namespace aip::rt
