// Test signals for `apo_host` -- what the synthetic audio engine feeds into an APO.
//
// The point of these is metering. A pass-through chain is provable with a ramp and a memcmp, but
// the moment a real client with real plugins is on the other side, the interesting questions are
// analogue ones: is the level right, is the tone where it should be, is a channel inverted, is
// something clipping. Those need a signal a meter can read, so this is a small extensible bank
// of them rather than one hard-coded generator.
//
// **Levels are peak dBFS, uniformly, for every generator.** A `-20` means the largest sample
// magnitude the signal will produce is 10^(-20/20) = 0.1. For a sine that is exact; for uniform
// white noise it is the bound of the distribution, so the RMS sits about 4.8 dB below it. The
// figure must be negative -- 0 dBFS peak is already full scale and anything above it clips
// on the way out.
//
// Adding a generator is one `SignalFactory` entry in `signal_source.cpp` and nothing else: the
// CLI, `--list-signals`, and the validation diagnostics are all driven off the table.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aip::tools {

/// Produces interleaved float blocks. Stateful: a tone keeps its phase across blocks, so a run
/// of blocks is one continuous signal rather than a stutter of restarts.
class SignalSource {
public:
    virtual ~SignalSource() = default;

    /// Writes `frames * channels` interleaved samples. Every channel gets the same signal --
    /// enough for metering, and a per-channel variant can be a new generator when something
    /// needs one.
    ///
    /// Allocation-free and non-blocking, so a generator can be called from the block loop
    /// without becoming the reason a deadline was missed.
    virtual void fill(float* interleaved, std::int32_t frames, std::uint32_t channels) noexcept = 0;

    /// One line naming the signal and its resolved parameters, for the run banner. What the tool
    /// prints has to be specific enough to put in a bug report.
    [[nodiscard]] virtual std::wstring describe() const = 0;
};

/// One entry in the bank.
struct SignalFactory {
    /// The name as typed, e.g. `sine`.
    const wchar_t* name;
    /// Argument shape, for `--list-signals` and for a parse error.
    const wchar_t* usage;
    /// One line on what it is for.
    const wchar_t* summary;
};

/// Every generator this build knows about, in the order `--list-signals` prints them.
[[nodiscard]] const std::vector<SignalFactory>& signalFactories();

/// Parses a spec such as `sine:1000`, `sine:1000:-12` or `noise:-20` and builds the generator.
///
/// `sampleRate` is needed because validation depends on it: a tone above Nyquist is not a tone,
/// it is an alias, and a tool that silently produced one would be lying to whatever is metering
/// the other end.
///
/// Returns null on any failure, with `error` set to something a user can act on. Never throws.
[[nodiscard]] std::unique_ptr<SignalSource> makeSignal(const std::wstring& spec,
                                                       std::uint32_t sampleRate,
                                                       std::wstring& error);

} // namespace aip::tools
