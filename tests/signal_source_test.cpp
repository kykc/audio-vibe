// The `apo_host` signal bank, and specifically its validation rules.
//
// The generators themselves are a few lines of arithmetic; what is worth testing is that a spec
// which cannot mean what it says is refused rather than silently reinterpreted. A tone above
// Nyquist comes out as its alias, and a level at or above 0 dBFS peak is already clipping -- in
// both cases the tool would go on to produce a signal, and whoever was reading a meter at the
// other end would be measuring the wrong thing without being told.

#include <catch2/catch_test_macros.hpp>

#include "signal_source.h"

#include <cmath>
#include <vector>

using namespace aip;

namespace {

constexpr std::uint32_t kRate = 48000;

/// Largest sample magnitude over a block, which is what a peak dBFS figure claims to bound.
float peakOf(tools::SignalSource& source, std::int32_t frames, std::uint32_t channels) {
    std::vector<float> block(static_cast<std::size_t>(frames) * channels, 0.0f);
    source.fill(block.data(), frames, channels);
    float peak = 0.0f;
    for (const float sample : block) {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

} // namespace

TEST_CASE("the signal bank is not empty and every entry is described", "[tools][signal]") {
    // `--list-signals` and every parse error are generated from this table, so an entry with a
    // hole in it is a user staring at a blank line.
    const auto& factories = tools::signalFactories();
    REQUIRE_FALSE(factories.empty());
    for (const tools::SignalFactory& factory : factories) {
        REQUIRE(factory.name != nullptr);
        REQUIRE(factory.usage != nullptr);
        REQUIRE(factory.summary != nullptr);
        REQUIRE(std::wstring(factory.name).size() > 0);
    }
}

TEST_CASE("an unknown signal name is refused with a usable message", "[tools][signal]") {
    std::wstring error;
    REQUIRE(tools::makeSignal(L"triangle:100", kRate, error) == nullptr);
    REQUIRE(error.find(L"triangle") != std::wstring::npos);
    REQUIRE(error.find(L"--list-signals") != std::wstring::npos);
}

TEST_CASE("noise takes a level below zero dBFS and nothing else", "[tools][signal]") {
    std::wstring error;

    REQUIRE(tools::makeSignal(L"noise:-20", kRate, error) != nullptr);
    REQUIRE(error.empty());

    // At 0 dBFS the bound of the distribution is full scale; above it, every sample at the bound
    // is past it. Both are refused, which is the rule as stated.
    REQUIRE(tools::makeSignal(L"noise:0", kRate, error) == nullptr);
    REQUIRE(tools::makeSignal(L"noise:3", kRate, error) == nullptr);

    REQUIRE(tools::makeSignal(L"noise", kRate, error) == nullptr);
    REQUIRE(tools::makeSignal(L"noise:-20:-20", kRate, error) == nullptr);
    REQUIRE(tools::makeSignal(L"noise:loud", kRate, error) == nullptr);

    // Half-parsed numbers are the dangerous case: a tool that took the 20 out of "20dB" would
    // run a test nobody asked for.
    REQUIRE(tools::makeSignal(L"noise:-20dB", kRate, error) == nullptr);
}

TEST_CASE("noise honours the peak level it was given", "[tools][signal]") {
    std::wstring error;
    auto source = tools::makeSignal(L"noise:-20", kRate, error);
    REQUIRE(source != nullptr);

    // -20 dBFS peak is an amplitude of 0.1. Uniform noise approaches its bound rather than
    // hitting it, so the assertion is "under, and not far under" over enough samples to be sure.
    const float peak = peakOf(*source, 48000, 2);
    REQUIRE(peak <= 0.1f);
    REQUIRE(peak > 0.09f);
}

TEST_CASE("noise is deterministic", "[tools][signal]") {
    // Two runs of the same command must produce the same samples, or a difference between two
    // runs of a soak test says nothing.
    std::wstring error;
    auto first = tools::makeSignal(L"noise:-6", kRate, error);
    auto second = tools::makeSignal(L"noise:-6", kRate, error);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    std::vector<float> a(960, 0.0f);
    std::vector<float> b(960, 0.0f);
    first->fill(a.data(), 480, 2);
    second->fill(b.data(), 480, 2);
    REQUIRE(a == b);
}

TEST_CASE("a sine outside 20 Hz to Nyquist is refused", "[tools][signal]") {
    std::wstring error;

    REQUIRE(tools::makeSignal(L"sine:1000", kRate, error) != nullptr);
    REQUIRE(tools::makeSignal(L"sine:20", kRate, error) != nullptr);
    REQUIRE(tools::makeSignal(L"sine:24000", kRate, error) != nullptr);

    REQUIRE(tools::makeSignal(L"sine:19", kRate, error) == nullptr);
    REQUIRE(tools::makeSignal(L"sine:0", kRate, error) == nullptr);
    REQUIRE(tools::makeSignal(L"sine:-100", kRate, error) == nullptr);

    // Just past Nyquist. This is the one that matters: it would otherwise produce a perfectly
    // clean tone at the wrong frequency.
    REQUIRE(tools::makeSignal(L"sine:24001", kRate, error) == nullptr);
    REQUIRE(error.find(L"Nyquist") != std::wstring::npos);
}

TEST_CASE("the Nyquist limit follows the sample rate", "[tools][signal]") {
    // The bound is not a constant, which is why the rate has to reach the factory at all.
    std::wstring error;
    REQUIRE(tools::makeSignal(L"sine:20000", 44100, error) != nullptr);
    REQUIRE(tools::makeSignal(L"sine:23000", 44100, error) == nullptr);
    REQUIRE(tools::makeSignal(L"sine:23000", 96000, error) != nullptr);
}

TEST_CASE("a sine peaks at the level it was given, and defaults below full scale", "[tools][signal]") {
    std::wstring error;

    auto quiet = tools::makeSignal(L"sine:1000:-12", kRate, error);
    REQUIRE(quiet != nullptr);
    const float peak = peakOf(*quiet, 4800, 1);
    // -12 dBFS is 0.2512. A whole number of cycles at 1 kHz in 100 ms, so the peak is reached.
    REQUIRE(peak <= 0.2512f + 1e-4f);
    REQUIRE(peak > 0.25f);

    auto defaulted = tools::makeSignal(L"sine:1000", kRate, error);
    REQUIRE(defaulted != nullptr);
    REQUIRE(peakOf(*defaulted, 4800, 1) < 1.0f);

    REQUIRE(tools::makeSignal(L"sine:1000:0", kRate, error) == nullptr);
    REQUIRE(tools::makeSignal(L"sine:1000:6", kRate, error) == nullptr);
}

TEST_CASE("a sine keeps its phase across blocks", "[tools][signal]") {
    // Filling block by block must produce the same samples as one long fill, or a run of blocks
    // is a buzz at the block rate rather than a tone -- and it would meter as one.
    std::wstring error;
    auto continuous = tools::makeSignal(L"sine:1000:-6", kRate, error);
    auto chunked = tools::makeSignal(L"sine:1000:-6", kRate, error);
    REQUIRE(continuous != nullptr);
    REQUIRE(chunked != nullptr);

    std::vector<float> whole(960, 0.0f);
    continuous->fill(whole.data(), 960, 1);

    std::vector<float> pieces(960, 0.0f);
    chunked->fill(pieces.data(), 480, 1);
    chunked->fill(pieces.data() + 480, 480, 1);

    for (std::size_t i = 0; i < whole.size(); ++i) {
        REQUIRE(std::fabs(whole[i] - pieces[i]) < 1e-6f);
    }
}

TEST_CASE("silence is silent and takes no parameters", "[tools][signal]") {
    std::wstring error;
    auto source = tools::makeSignal(L"silence", kRate, error);
    REQUIRE(source != nullptr);
    REQUIRE(peakOf(*source, 480, 2) == 0.0f);

    REQUIRE(tools::makeSignal(L"silence:-20", kRate, error) == nullptr);
}
