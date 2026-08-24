// Fixed-capacity lock-free single-producer/single-consumer queue -- the control-plane <-> audio
// thread transport required by design_doc.md sec. 7.4.2.
//
// The predecessor's `TomatlVst/spsc_queue.h` was a linked-node design that allocated on the
// producer side; this is a ring buffer over inline storage, so neither side ever touches the
// heap. Capacity is a compile-time power of two; one slot is reserved to distinguish full from
// empty, so `capacity()` usable elements are available.
//
// `drain()` exists to make the "bounded work per block" rule (sec. 7.4.2) the easy thing to write:
// a backlogged queue is drained a fixed maximum per block rather than caught up in one go.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace aip::rt {

#if defined(_MSC_VER)
// C4324: "structure was padded due to alignment specifier". That padding is the entire point --
// head_ and tail_ are written by different threads and must not share a cache line -- so the
// warning is suppressed here rather than project-wide.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <typename T, std::size_t Slots>
class SpscQueue {
    static_assert(Slots >= 2, "need at least two slots");
    static_assert((Slots & (Slots - 1)) == 0, "Slots must be a power of two");
    static_assert(std::is_nothrow_default_constructible_v<T>,
        "storage is default-constructed up front; T must not throw or allocate");
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
        "push/pop must not throw on the audio thread");

public:
    static constexpr std::size_t capacity() noexcept { return Slots - 1; }

    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Producer side. Returns false when full -- the caller decides what to drop.
    template <typename U>
    [[nodiscard]] bool push(U&& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        slots_[head] = std::forward<U>(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false when empty.
    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = std::move(slots_[tail]);
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    /// Consumer side, bounded: applies `fn` to at most `maxItems` elements and returns how many
    /// were consumed. This is the shape sec. 7.4.2 asks for on the audio thread.
    template <typename Fn>
    std::size_t drain(std::size_t maxItems, Fn&& fn) noexcept {
        std::size_t consumed = 0;
        T item{};
        while (consumed < maxItems && pop(item)) {
            fn(item);
            ++consumed;
        }
        return consumed;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & kMask;
    }

private:
    static constexpr std::size_t kMask = Slots - 1;
#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t kCacheLine = 64;
#endif

    alignas(kCacheLine) std::atomic<std::size_t> head_{0}; // written by the producer only
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0}; // written by the consumer only
    alignas(kCacheLine) std::array<T, Slots> slots_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace aip::rt
