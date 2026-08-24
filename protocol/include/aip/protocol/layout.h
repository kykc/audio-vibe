// Protocol v1 -- shared memory layout and named objects.
//
// NORMATIVE. This header is the single source of truth for the wire format described in
// design_doc.md sec. 4, and sec. 4 is frozen: the client must interoperate with the *existing*,
// unmodified APO binary. Deviations here are defects, not improvements -- improvements belong
// to a future v2 (sec. 9.1).
//
// Header-only and free of Windows headers on purpose, so the same definitions serve the
// client, the future APO, and the conformance tests (sec. 7.1).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aip::protocol {

/// Size of the shared file mapping. Exactly 1 MiB -- fixed by sec. 4.2.
inline constexpr std::size_t kMappingSize = 1u * 1024u * 1024u;

/// Object name prefix. The endpoint GUID as returned by `PKEY_AudioEndpoint_GUID` is appended
/// verbatim, *including braces* (sec. 4.2). The APO lowercases a separate copy for registry
/// lookups only; that does not affect these names.
inline constexpr std::wstring_view kNamePrefix = L"Global\\TOMATL.AUDIO.IPC.";
inline constexpr std::wstring_view kKingEventSuffix = L".KING";
inline constexpr std::wstring_view kValetEventSuffix = L".VALET";

/// The 16-byte header at the start of the mapping (sec. 4.3). Packed, little-endian, no padding.
struct SharedHeader {
    /// 0 means "no client attached". Written by the valet to claim, by the king to evict.
    std::uint32_t valetId;
    /// Written by the king. May be stale after a sample-rate-only format change -- see the
    /// `smartOpen` bug in sec. 3.7.3. Re-read it every block; never cache across blocks (sec. 4.5).
    std::uint32_t sampleRate;
    /// Written by the king.
    std::uint32_t channelCount;
    /// **Total** sample count for the block, i.e. `frames * channelCount` -- not a frame count.
    std::int32_t size;
};

static_assert(sizeof(SharedHeader) == 16, "protocol v1 header is exactly 16 bytes (sec. 4.3)");
static_assert(alignof(SharedHeader) == 4, "protocol v1 header fields are 4-byte aligned");
static_assert(offsetof(SharedHeader, valetId) == 0);
static_assert(offsetof(SharedHeader, sampleRate) == 4);
static_assert(offsetof(SharedHeader, channelCount) == 8);
static_assert(offsetof(SharedHeader, size) == 12);

inline constexpr std::size_t kHeaderSize = sizeof(SharedHeader);

/// Audio payload capacity: (1 MiB - 16) / 4 = 262,140 floats, i.e. 32,767 frames at 8
/// channels (sec. 4.3). Real block sizes are three orders of magnitude smaller.
inline constexpr std::int32_t kMaxPayloadSamples =
    static_cast<std::int32_t>((kMappingSize - kHeaderSize) / sizeof(float));

static_assert(kMaxPayloadSamples == 262140, "sec. 4.3 states the payload capacity explicitly");

/// A `valetId` of zero means "no client attached"; a valet must never claim with it (sec. 4.4).
inline constexpr std::uint32_t kNoValet = 0u;

/// Builds the base object name for an endpoint GUID. Use the GUID *verbatim*, braces included.
inline std::wstring objectBaseName(std::wstring_view endpointGuid) {
    std::wstring name;
    name.reserve(kNamePrefix.size() + endpointGuid.size() + kValetEventSuffix.size());
    name.append(kNamePrefix);
    name.append(endpointGuid);
    return name;
}

inline std::wstring kingEventName(std::wstring_view base) { return std::wstring(base).append(kKingEventSuffix); }

inline std::wstring valetEventName(std::wstring_view base) { return std::wstring(base).append(kValetEventSuffix); }

} // namespace aip::protocol
