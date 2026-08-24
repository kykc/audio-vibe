// Protocol v1 wire-format conformance: names, layout, limits (design_doc.md sec. 4.2, sec. 4.3).
//
// These assertions restate the normative text. If one of them fails, either the code drifted or
// somebody changed a frozen decision -- in both cases the fix is in the code, not the test.

#include "aip/protocol/header_access.h"
#include "aip/protocol/layout.h"
#include "aip/protocol/planar.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace aip;

TEST_CASE("shared memory layout matches section 4.3", "[protocol][layout]") {
    STATIC_REQUIRE(sizeof(protocol::SharedHeader) == 16);
    STATIC_REQUIRE(offsetof(protocol::SharedHeader, valetId) == 0);
    STATIC_REQUIRE(offsetof(protocol::SharedHeader, sampleRate) == 4);
    STATIC_REQUIRE(offsetof(protocol::SharedHeader, channelCount) == 8);
    STATIC_REQUIRE(offsetof(protocol::SharedHeader, size) == 12);

    STATIC_REQUIRE(protocol::kMappingSize == 1024u * 1024u);
    STATIC_REQUIRE(protocol::kMaxPayloadSamples == 262140);
    // Sec. 4.3 states the capacity as 32,767 frames at 8 channels.
    STATIC_REQUIRE(protocol::kMaxPayloadSamples / 8 == 32767);
}

TEST_CASE("object names match section 4.2", "[protocol][names]") {
    // The endpoint GUID is used verbatim, braces included.
    const std::wstring guid = L"{0.0.0.00000000}.{2a1b7f4e-9c3d-4f8a-b0e1-3c5d6e7f8a9b}";
    const std::wstring base = protocol::objectBaseName(guid);

    REQUIRE(base == L"Global\\TOMATL.AUDIO.IPC." + guid);
    REQUIRE(protocol::kingEventName(base) == base + L".KING");
    REQUIRE(protocol::valetEventName(base) == base + L".VALET");

    // Braces are not stripped and the case is not folded: only the APO's registry lookups use a
    // lowercased copy, and that does not affect these names.
    REQUIRE(base.find(L'{') != std::wstring::npos);
    REQUIRE(base.find(L"2a1b7f4e") != std::wstring::npos);
}

TEST_CASE("header access reads and writes the documented offsets", "[protocol][layout]") {
    std::array<unsigned char, protocol::kHeaderSize + 4 * sizeof(float)> raw{};
    protocol::HeaderAccess header(raw.data());

    header.setValetId(0xDEADBEEFu);
    header.setSampleRate(48000u);
    header.setChannelCount(2u);
    header.setSize(4);

    // Read back through the raw bytes: the point is the byte offsets, not the accessors.
    std::uint32_t valetId = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::int32_t size = 0;
    std::memcpy(&valetId, raw.data() + 0, 4);
    std::memcpy(&sampleRate, raw.data() + 4, 4);
    std::memcpy(&channelCount, raw.data() + 8, 4);
    std::memcpy(&size, raw.data() + 12, 4);

    REQUIRE(valetId == 0xDEADBEEFu);
    REQUIRE(sampleRate == 48000u);
    REQUIRE(channelCount == 2u);
    REQUIRE(size == 4);
    REQUIRE(header.payload() == reinterpret_cast<float*>(raw.data() + 16));
}

TEST_CASE("block validation rejects what section 4.3 makes impossible", "[protocol][layout]") {
    using protocol::HeaderStatus;

    REQUIRE(protocol::validateBlock(2, 480) == HeaderStatus::Ok);
    REQUIRE(protocol::validateBlock(2, 0) == HeaderStatus::Ok); // an empty block is well formed

    // At 2 channels a block can fill the payload exactly. At 8 it cannot: 262,140 is not a
    // multiple of 8, so the largest well-formed 8-channel block is the 32,767 frames sec. 4.3
    // quotes -- 262,136 samples -- and the four trailing floats are simply unusable.
    REQUIRE(protocol::validateBlock(2, protocol::kMaxPayloadSamples) == HeaderStatus::Ok);
    REQUIRE(protocol::validateBlock(8, 32767 * 8) == HeaderStatus::Ok);
    REQUIRE(protocol::validateBlock(8, protocol::kMaxPayloadSamples) == HeaderStatus::SizeNotDivisibleByChannelCount);

    REQUIRE(protocol::validateBlock(0, 480) == HeaderStatus::ZeroChannelCount);
    REQUIRE(protocol::validateBlock(2, -1) == HeaderStatus::NegativeSize);
    REQUIRE(protocol::validateBlock(2, protocol::kMaxPayloadSamples + 2) == HeaderStatus::SizeExceedsCapacity);
    REQUIRE(protocol::validateBlock(2, 481) == HeaderStatus::SizeNotDivisibleByChannelCount);
}
