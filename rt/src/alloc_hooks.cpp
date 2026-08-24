// Global operator new / operator delete replacements for the real-time violation detector
// (design_doc.md sec. 7.4.6). Every allocation and deallocation reached from inside a
// RealtimeGuard section is counted; outside one they are plain malloc/free forwarders.
//
// This translation unit is built as a CMake OBJECT library (`aip_rt_hooks`) rather than a
// static library on purpose: replacement operators must always be linked into the final image,
// and a static library's members are only pulled in when some symbol from them is referenced.
//
// Allocation and the matching deallocation must go through the same pair of CRT entry points,
// so the plain overloads use malloc/free and the aligned overloads use _aligned_malloc/
// _aligned_free -- exactly as the MSVC defaults do, which keeps us interoperable with memory
// allocated inside DLLs that carry their own operators (Qt, and later the VST3 plugins).

#include "aip/rt/realtime_guard.h"

#include <cstdlib>
#include <new>

#if defined(AIP_RT_CHECKS)

namespace {

void* allocate(std::size_t bytes) noexcept {
    aip::rt::noteAllocation();
    // malloc(0) may return nullptr, which operator new must not do for a zero-size request.
    return std::malloc(bytes != 0 ? bytes : 1);
}

void* allocateAligned(std::size_t bytes, std::size_t alignment) noexcept {
    aip::rt::noteAllocation();
    return _aligned_malloc(bytes != 0 ? bytes : 1, alignment);
}

void release(void* p) noexcept {
    aip::rt::noteDeallocation();
    std::free(p);
}

void releaseAligned(void* p) noexcept {
    aip::rt::noteDeallocation();
    _aligned_free(p);
}

} // namespace

void* operator new(std::size_t bytes) {
    if (void* p = allocate(bytes)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) {
    if (void* p = allocate(bytes)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept { return allocate(bytes); }

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept { return allocate(bytes); }

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    if (void* p = allocateAligned(bytes, static_cast<std::size_t>(alignment))) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    if (void* p = allocateAligned(bytes, static_cast<std::size_t>(alignment))) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}

void operator delete(void* p) noexcept { release(p); }

void operator delete[](void* p) noexcept { release(p); }

void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }

void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

void operator delete(void* p, std::size_t) noexcept { release(p); }

void operator delete[](void* p, std::size_t) noexcept { release(p); }

void operator delete(void* p, std::align_val_t) noexcept { releaseAligned(p); }

void operator delete[](void* p, std::align_val_t) noexcept { releaseAligned(p); }

void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }

void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }

void operator delete(void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }

#endif // AIP_RT_CHECKS
