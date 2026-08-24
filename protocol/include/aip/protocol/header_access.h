// Typed, tear-free access to the shared header of protocol v1 (design_doc.md sec. 4.3).
//
// Keeps every offset in one place and, by routing each field through `std::atomic_ref`, keeps
// the compiler from tearing, hoisting or coalescing accesses to memory another process is
// writing concurrently. Relaxed ordering is correct here: the KING/VALET event rendezvous
// (sec. 4.4) is what orders the payload against the header, not these loads.
//
// Every member is allocation-free and safe on the audio thread (sec. 7.4.4 permits reads and writes
// through the mapped view once it has been mapped and touched at stream open).

#pragma once

#include "aip/protocol/layout.h"
#include "aip/protocol/planar.h"

#include <atomic>
#include <cassert>
#include <cstdint>

namespace aip::protocol {

class HeaderAccess {
public:
    HeaderAccess() = default;

    explicit HeaderAccess(void* mappingBase) noexcept : header_(static_cast<SharedHeader*>(mappingBase)) {}

    [[nodiscard]] bool valid() const noexcept { return header_ != nullptr; }

    [[nodiscard]] std::uint32_t valetId() const noexcept { return load(header_->valetId); }

    /// Attach step 5 / eviction / clean detach (sec. 4.4). The write is unconditional by design:
    /// that is exactly how a newly arriving valet displaces the incumbent (sec. 4.1).
    void setValetId(std::uint32_t id) noexcept { store(header_->valetId, id); }

    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return load(header_->sampleRate); }

    [[nodiscard]] std::uint32_t channelCount() const noexcept { return load(header_->channelCount); }

    /// Total sample count, i.e. `frames * channelCount` (sec. 4.3).
    [[nodiscard]] std::int32_t size() const noexcept { return load(header_->size); }

    void setSampleRate(std::uint32_t v) noexcept { store(header_->sampleRate, v); }

    void setChannelCount(std::uint32_t v) noexcept { store(header_->channelCount, v); }

    void setSize(std::int32_t v) noexcept { store(header_->size, v); }

    /// Start of the planar audio payload, at offset 16.
    [[nodiscard]] float* payload() const noexcept {
        return reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(header_) + kHeaderSize);
    }

    /// Convenience: a planar view over the payload using the header's current geometry. Call
    /// `validateBlock` first -- a malformed header must not be turned into a view.
    [[nodiscard]] PlanarView planarView() const noexcept { return PlanarView(payload(), channelCount(), size()); }

private:
    template <typename T>
    static T load(T& field) noexcept {
        return std::atomic_ref<T>(field).load(std::memory_order_relaxed);
    }

    template <typename T>
    static void store(T& field, T value) noexcept {
        std::atomic_ref<T>(field).store(value, std::memory_order_relaxed);
    }

    SharedHeader* header_ = nullptr;
};

} // namespace aip::protocol
