// Planar payload addressing (design_doc.md sec. 4.3): sample `s` of channel `c` lives at
// float index `c * (size / channelCount) + s`, and `size` is a total sample count, not a
// frame count.
//
// Getting this wrong produces audio that is subtly channel-smeared rather than obviously broken,
// which is why it gets its own test rather than being left to the round-trip test alone.

#include "aip/protocol/planar.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <vector>

using namespace aip;

TEST_CASE("planar view addresses channels per section 4.3", "[protocol][planar]") {
    // 3 frames, 2 channels: size == 6, perChannel == 3.
    std::vector<float> payload{10.f, 11.f, 12.f, 20.f, 21.f, 22.f};
    protocol::PlanarView view(payload.data(), 2, 6);

    REQUIRE(view.valid());
    REQUIRE(view.channelCount() == 2);
    REQUIRE(view.sampleCount() == 6);
    REQUIRE(view.frameCount() == 3);
    REQUIRE(view.channel(0) == payload.data() + 0);
    REQUIRE(view.channel(1) == payload.data() + 3);
    REQUIRE(view.channel(1)[2] == 22.f);
}

TEST_CASE("interleave conversion round-trips exactly", "[protocol][planar]") {
    const std::uint32_t channelCount = GENERATE(1u, 2u, 6u, 8u);
    const std::int32_t frames = GENERATE(1, 16, 480, 1024);
    const std::int32_t size = frames * static_cast<std::int32_t>(channelCount);

    std::vector<float> interleaved(static_cast<std::size_t>(size));
    for (std::int32_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            // Distinct per (channel, frame) so a channel swap or an off-by-one cannot pass.
            interleaved[static_cast<std::size_t>(frame * channelCount + ch)] =
                static_cast<float>(ch) * 100000.f + static_cast<float>(frame);
        }
    }

    std::vector<float> planar(static_cast<std::size_t>(size));
    protocol::deinterleave(interleaved.data(), planar.data(), size, channelCount);

    // Spot-check the layout directly, not just the round trip.
    protocol::PlanarView view(planar.data(), channelCount, size);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        for (std::int32_t frame = 0; frame < frames; ++frame) {
            REQUIRE(view.channel(ch)[frame] ==
                    static_cast<float>(ch) * 100000.f + static_cast<float>(frame));
        }
    }

    std::vector<float> restored(static_cast<std::size_t>(size), -1.f);
    protocol::reinterleave(planar.data(), restored.data(), size, channelCount);
    REQUIRE(restored == interleaved);
}
