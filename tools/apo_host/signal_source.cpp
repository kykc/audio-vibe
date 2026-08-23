#include "signal_source.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <sstream>

namespace aip::tools {

namespace {

/// Peak dBFS to linear amplitude. `-20` -> 0.1, `-6` -> ~0.501, `0` -> 1.0.
[[nodiscard]] float amplitudeFromDbfs(double dbfs) noexcept {
    return static_cast<float>(std::pow(10.0, dbfs / 20.0));
}

/// Splits `sine:1000:-12` into `sine` and `{1000, -12}`. Colons rather than commas because a
/// comma is what a shell and a CSV both want to own.
void splitSpec(const std::wstring& spec, std::wstring& name, std::vector<std::wstring>& params) {
    std::wstringstream stream(spec);
    std::wstring part;
    bool first = true;
    while (std::getline(stream, part, L':')) {
        if (first) {
            name = part;
            first = false;
        } else {
            params.push_back(part);
        }
    }
}

/// Strict: the whole token must be a number. `std::stod` would accept `1000abc` and a tool that
/// quietly took the `1000` would be a tool that ran the wrong test.
[[nodiscard]] bool parseNumber(const std::wstring& text, double& out) {
    if (text.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return false;
    }
    if (!std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

// -----------------------------------------------------------------------------------------
// The generators
// -----------------------------------------------------------------------------------------

class SilenceSource final : public SignalSource {
public:
    void fill(float* interleaved, std::int32_t frames, std::uint32_t channels) noexcept override {
        const auto count = static_cast<std::size_t>(frames) * channels;
        for (std::size_t i = 0; i < count; ++i) {
            interleaved[i] = 0.0f;
        }
    }

    [[nodiscard]] std::wstring describe() const override { return L"silence"; }
};

/// Uniform white noise, deterministic.
///
/// Deterministic on purpose: two runs of the same command produce the same samples, so a
/// difference between them is a difference in the code under test rather than in the dice. The
/// generator is xorshift32 -- flat enough for a level and spectrum check, allocation-free, and
/// not pretending to be a cryptographic or a Gaussian source.
class NoiseSource final : public SignalSource {
public:
    explicit NoiseSource(double peakDbfs) noexcept
        : peakDbfs_(peakDbfs), amplitude_(amplitudeFromDbfs(peakDbfs)) {}

    void fill(float* interleaved, std::int32_t frames, std::uint32_t channels) noexcept override {
        for (std::int32_t f = 0; f < frames; ++f) {
            // One draw per frame, shared across channels: correlated channels, which is what a
            // level check wants. Independent channels would be a different generator.
            const float sample = next() * amplitude_;
            for (std::uint32_t c = 0; c < channels; ++c) {
                interleaved[static_cast<std::size_t>(f) * channels + c] = sample;
            }
        }
    }

    [[nodiscard]] std::wstring describe() const override {
        std::wostringstream out;
        out << L"white noise at " << peakDbfs_ << L" dBFS peak (amplitude " << amplitude_ << L")";
        return out.str();
    }

private:
    /// Uniform in [-1, 1).
    [[nodiscard]] float next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        // Top 24 bits: float has 24 bits of mantissa, so anything below is noise about noise.
        const auto unit = static_cast<float>(state_ >> 8) / static_cast<float>(1u << 24);
        return unit * 2.0f - 1.0f;
    }

    double peakDbfs_;
    float amplitude_;
    std::uint32_t state_ = 0x9E3779B9u;
};

class SineSource final : public SignalSource {
public:
    SineSource(double hz, double peakDbfs, std::uint32_t sampleRate) noexcept
        : hz_(hz), peakDbfs_(peakDbfs), amplitude_(amplitudeFromDbfs(peakDbfs)),
          step_(2.0 * std::numbers::pi * hz / static_cast<double>(sampleRate)) {}

    void fill(float* interleaved, std::int32_t frames, std::uint32_t channels) noexcept override {
        for (std::int32_t f = 0; f < frames; ++f) {
            const float sample = amplitude_ * static_cast<float>(std::sin(phase_));
            phase_ += step_;
            // Wrapped every block rather than left to grow: at 48 kHz an unwrapped phase loses
            // its low bits within minutes, and this tool is meant for soak runs.
            if (phase_ >= 2.0 * std::numbers::pi) {
                phase_ -= 2.0 * std::numbers::pi;
            }
            for (std::uint32_t c = 0; c < channels; ++c) {
                interleaved[static_cast<std::size_t>(f) * channels + c] = sample;
            }
        }
    }

    [[nodiscard]] std::wstring describe() const override {
        std::wostringstream out;
        out << L"sine " << hz_ << L" Hz at " << peakDbfs_ << L" dBFS peak";
        return out.str();
    }

private:
    double hz_;
    double peakDbfs_;
    float amplitude_;
    double step_;
    double phase_ = 0.0;
};

// -----------------------------------------------------------------------------------------
// The bank
// -----------------------------------------------------------------------------------------

/// Default level for a tone when the spec does not give one. Well clear of full scale, so a
/// plugin with a few dB of gain in it does not clip before anyone has looked at a meter.
constexpr double kDefaultSineDbfs = -6.0;

[[nodiscard]] std::unique_ptr<SignalSource> makeSilence(const std::vector<std::wstring>& params,
                                                        std::uint32_t, std::wstring& error) {
    if (!params.empty()) {
        error = L"silence takes no parameters";
        return nullptr;
    }
    return std::make_unique<SilenceSource>();
}

[[nodiscard]] std::unique_ptr<SignalSource> makeNoise(const std::vector<std::wstring>& params,
                                                      std::uint32_t, std::wstring& error) {
    if (params.size() != 1) {
        error = L"noise needs exactly one parameter: the peak level in dBFS, e.g. noise:-20";
        return nullptr;
    }
    double dbfs = 0.0;
    if (!parseNumber(params[0], dbfs)) {
        error = L"noise: '" + params[0] + L"' is not a number";
        return nullptr;
    }
    // The rule from the request, and it is a real one rather than a formality: at or above
    // 0 dBFS every sample at the bound is already at or past full scale.
    if (dbfs >= 0.0) {
        error = L"noise: the level must be below 0 dBFS (got " + params[0] + L")";
        return nullptr;
    }
    return std::make_unique<NoiseSource>(dbfs);
}

[[nodiscard]] std::unique_ptr<SignalSource> makeSine(const std::vector<std::wstring>& params,
                                                     std::uint32_t sampleRate,
                                                     std::wstring& error) {
    if (params.empty() || params.size() > 2) {
        error = L"sine needs a frequency in Hz and optionally a peak level in dBFS, "
                L"e.g. sine:1000 or sine:1000:-12";
        return nullptr;
    }
    double hz = 0.0;
    if (!parseNumber(params[0], hz)) {
        error = L"sine: '" + params[0] + L"' is not a number";
        return nullptr;
    }
    // 20 Hz at the bottom because below it there is nothing to hear and nothing to meter; Nyquist
    // at the top because above it the tone is not the tone, it is its alias -- and a test signal
    // that is not what it says it is defeats the whole purpose of having one.
    const double nyquist = static_cast<double>(sampleRate) / 2.0;
    if (hz < 20.0 || hz > nyquist) {
        std::wostringstream out;
        out << L"sine: " << hz << L" Hz is outside 20 Hz to " << nyquist
            << L" Hz (Nyquist at " << sampleRate << L" Hz)";
        error = out.str();
        return nullptr;
    }

    double dbfs = kDefaultSineDbfs;
    if (params.size() == 2) {
        if (!parseNumber(params[1], dbfs)) {
            error = L"sine: '" + params[1] + L"' is not a number";
            return nullptr;
        }
        if (dbfs >= 0.0) {
            error = L"sine: the level must be below 0 dBFS (got " + params[1] + L")";
            return nullptr;
        }
    }
    return std::make_unique<SineSource>(hz, dbfs, sampleRate);
}

using Builder = std::unique_ptr<SignalSource> (*)(const std::vector<std::wstring>&, std::uint32_t,
                                                  std::wstring&);

struct Entry {
    SignalFactory info;
    Builder build;
};

const std::vector<Entry>& entries() {
    static const std::vector<Entry> table = {
        {{L"silence", L"silence", L"digital black -- the baseline a level check is measured against"},
         &makeSilence},
        {{L"noise", L"noise:<peak dBFS>",
          L"deterministic uniform white noise, for level and broadband checks"},
         &makeNoise},
        {{L"sine", L"sine:<Hz>[:<peak dBFS>]",
          L"continuous tone, phase-continuous across blocks"},
         &makeSine},
    };
    return table;
}

} // namespace

const std::vector<SignalFactory>& signalFactories() {
    static const std::vector<SignalFactory> infos = [] {
        std::vector<SignalFactory> out;
        for (const Entry& entry : entries()) {
            out.push_back(entry.info);
        }
        return out;
    }();
    return infos;
}

std::unique_ptr<SignalSource> makeSignal(const std::wstring& spec, std::uint32_t sampleRate,
                                         std::wstring& error) {
    error.clear();
    if (sampleRate == 0) {
        error = L"sample rate must be nonzero";
        return nullptr;
    }

    std::wstring name;
    std::vector<std::wstring> params;
    splitSpec(spec, name, params);

    for (const Entry& entry : entries()) {
        if (name == entry.info.name) {
            return entry.build(params, sampleRate, error);
        }
    }

    error = L"unknown signal '" + name + L"'; try --list-signals";
    return nullptr;
}

} // namespace aip::tools
