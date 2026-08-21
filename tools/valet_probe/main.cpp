// valet_probe -- a console client that attaches to the *deployed, unmodified* APO as a protocol
// v1 valet (design_doc.md sec. 7.3) and reports what it sees.
//
// This is the bridge between the conformance harness and real hardware. The harness proves the
// client against a synthetic king; this proves it against `audiodg.exe`, which is the only thing
// that can confirm the object names, the endpoint GUID form and the block geometry are right.
//
// Everything here is control-plane code: enumeration, console output, argument parsing. The
// audio path is entirely inside ipc/ and is never touched from this file.
//
//   valet_probe                 attach to the default render endpoint
//   valet_probe --list          list active render endpoints and exit
//   valet_probe --endpoint N    attach to the Nth endpoint from --list
//   valet_probe --gain 0.5      apply a gain instead of passing through
//   valet_probe --plugin P      run a VST3 plugin chain instead of a gain; repeatable
//   valet_probe --seconds 10    run for a fixed time instead of until Ctrl+C
//
// `--plugin` is the only mode in which anything from `engine/` is involved, and it is mutually
// exclusive with `--gain`: they are different BlockProcessors and only one can be installed. The
// chain cannot be built up front, because protocol v1 announces the endpoint's format nowhere --
// the geometry is known only once a block has been through (sec. 4.5). The poll loop therefore
// calls Engine::serviceFormatChange, which builds on the first observation and rebuilds if
// Windows changes the format underneath.

#include "aip/engine/engine.h"
#include "aip/ipc/endpoints.h"
#include "aip/ipc/valet_supervisor.h"
#include "aip/ipc/valet_thread.h"
#include "aip/protocol/layout.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace aip;

namespace {

std::atomic<bool> gStopRequested{false};

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        gStopRequested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

/// Scales every sample by a gain the control thread publishes. Real-time safe (sec. 7.4.1): one
/// relaxed atomic load and a loop over preallocated shared memory, no heap, no locks.
class GainProcessor final : public ipc::BlockProcessor {
public:
    explicit GainProcessor(float gain) : gain_(gain) {}

    void processBlock(ipc::BlockInfo& block) noexcept override {
        const float gain = gain_.load(std::memory_order_relaxed);
        if (gain == 1.0f) {
            return; // pass-through: leave the payload exactly as the king wrote it
        }
        const std::int32_t frames = block.audio.frameCount();
        for (std::uint32_t ch = 0; ch < block.channelCount; ++ch) {
            float* samples = block.audio.channel(ch);
            for (std::int32_t s = 0; s < frames; ++s) {
                samples[s] *= gain;
            }
        }
    }

private:
    std::atomic<float> gain_;
};

const char* stateName(ipc::LinkState state) {
    switch (state) {
    case ipc::LinkState::Detached:
        return "detached";
    case ipc::LinkState::Attached:
        return "attached";
    case ipc::LinkState::Relinquished:
        return "relinquished";
    }
    return "?";
}

const char* exitReasonName(ipc::ValetExitReason reason) {
    switch (reason) {
    case ipc::ValetExitReason::None:
        return "none";
    case ipc::ValetExitReason::Stopped:
        return "stopped";
    case ipc::ValetExitReason::Stolen:
        return "stolen by another client";
    case ipc::ValetExitReason::Failed:
        return "wait failed (king went away)";
    }
    return "?";
}

void printEndpoints(const std::vector<ipc::RenderEndpoint>& endpoints) {
    if (endpoints.empty()) {
        std::puts("No active render endpoints found.");
        return;
    }
    std::printf("Active render endpoints:\n");
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        const ipc::RenderEndpoint& endpoint = endpoints[i];
        std::printf("  [%zu]%s %ls\n", i, endpoint.isDefault ? " (default)" : "",
                    endpoint.friendlyName.c_str());
        std::printf("        guid: %ls\n", endpoint.guid.c_str());
        std::printf("        base: %ls\n", protocol::objectBaseName(endpoint.guid).c_str());
    }
}

struct Options {
    bool list = false;
    int endpointIndex = -1; // -1 means "the default endpoint"
    float gain = 1.0f;
    int seconds = 0; // 0 means "until Ctrl+C"
    std::vector<std::string> plugins;
};

const char* kUsage =
    "Usage: valet_probe [--list] [--endpoint N] [--gain G] [--plugin PATH]... [--seconds S]";

bool parseOptions(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool hasValue = i + 1 < argc;

        if (std::strcmp(arg, "--list") == 0) {
            out.list = true;
        } else if (std::strcmp(arg, "--endpoint") == 0 && hasValue) {
            out.endpointIndex = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--gain") == 0 && hasValue) {
            out.gain = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(arg, "--plugin") == 0 && hasValue) {
            out.plugins.emplace_back(argv[++i]);
        } else if (std::strcmp(arg, "--seconds") == 0 && hasValue) {
            out.seconds = std::atoi(argv[++i]);
        } else {
            std::printf("Unrecognised argument: %s\n", arg);
            std::puts(kUsage);
            return false;
        }
    }

    if (!out.plugins.empty() && out.gain != 1.0f) {
        std::puts("--gain and --plugin are mutually exclusive: only one is installed.");
        std::puts(kUsage);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    // COM for MMDevice enumeration. Control thread only -- COM activation is forbidden on the
    // audio thread (sec. 7.4.1).
    ipc::ComApartment com;
    if (!com.ok()) {
        std::puts("Failed to initialise COM.");
        return 1;
    }

    const std::vector<ipc::RenderEndpoint> endpoints = ipc::enumerateRenderEndpoints();

    if (options.list) {
        printEndpoints(endpoints);
        return 0;
    }

    ipc::RenderEndpoint target;
    if (options.endpointIndex >= 0) {
        if (static_cast<std::size_t>(options.endpointIndex) >= endpoints.size()) {
            std::printf("No endpoint at index %d.\n", options.endpointIndex);
            printEndpoints(endpoints);
            return 1;
        }
        target = endpoints[static_cast<std::size_t>(options.endpointIndex)];
    } else {
        const auto defaultEndpoint = ipc::defaultRenderEndpoint();
        if (!defaultEndpoint) {
            std::puts("No default render endpoint.");
            return 1;
        }
        target = *defaultEndpoint;
    }

    std::printf("Endpoint : %ls\n", target.friendlyName.c_str());
    std::printf("GUID     : %ls\n", target.guid.c_str());
    std::printf("Objects  : %ls\n", protocol::objectBaseName(target.guid).c_str());
    if (options.plugins.empty()) {
        std::printf("Gain     : %.3f\n", static_cast<double>(options.gain));
    } else {
        for (const std::string& path : options.plugins) {
            std::printf("Plugin   : %s\n", path.c_str());
        }
    }
    std::puts("");
    std::puts("Waiting for the APO. If nothing attaches, the APO is not registered for this");
    std::puts("endpoint, or the endpoint has no active stream (sec. 4.4 step 1).");
    std::puts("Press Ctrl+C to stop.");
    std::puts("");

    ::SetConsoleCtrlHandler(consoleHandler, TRUE);

    // Both are constructed either way; only one is installed. An unused Engine loads nothing
    // and publishes nothing.
    GainProcessor gainProcessor(options.gain);
    engine::Engine host;

    for (const std::string& path : options.plugins) {
        std::string error;
        if (!host.appendPlugin(path, error)) {
            std::printf("Failed to load %s: %s\n", path.c_str(), error.c_str());
            return 1;
        }
    }

    ipc::BlockProcessor& processor = options.plugins.empty()
                                         ? static_cast<ipc::BlockProcessor&>(gainProcessor)
                                         : host.blockProcessor();
    ipc::ValetSupervisor supervisor(target.guid, processor);

    supervisor.setStateCallback([](ipc::LinkState state, ipc::ValetExitReason reason) {
        std::printf("[link] %s (%s)\n", stateName(state), exitReasonName(reason));
        std::fflush(stdout);
    });

    supervisor.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    ipc::ValetCounters::Snapshot previous{};

    // The chain can only be built once a block has revealed the format (sec. 4.5), so the sooner
    // the control thread looks, the less audio goes through unprocessed on every attach. Ticking
    // ten times a second and printing every tenth tick keeps that latency down to one block or
    // two without turning the report into a wall of text.
    constexpr int kTickMs = 100;
    constexpr int kTicksPerReport = 10;
    int tick = 0;

    while (!gStopRequested.load(std::memory_order_acquire)) {
        if (options.seconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (supervisor.state() == ipc::LinkState::Relinquished) {
            std::puts("Another client took the stream over; nothing left to do.");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));

        if (!options.plugins.empty()) {
            // Control-plane servicing. Both calls are no-ops when there is nothing to do, and
            // neither may ever happen on the valet thread (sec. 7.4.3).
            std::string error;
            if (host.serviceFormatChange(error)) {
                const engine::StreamFormat built = host.builtFormat();
                std::printf("[chain] built for %u Hz x%u ch, up to %d frames\n",
                            built.sampleRate, built.channelCount, built.maxFrames);
                std::fflush(stdout);
            } else if (!error.empty()) {
                std::printf("[chain] not built: %s\n", error.c_str());
                std::fflush(stdout);
            }
            host.serviceParameterEdits();
        }

        if (++tick < kTicksPerReport) {
            continue;
        }
        tick = 0;

        const ipc::ValetCounters::Snapshot now = supervisor.counters().snapshot();
        std::printf("blocks %llu (+%llu/s)  timeouts %llu  malformed %llu  reclaims %llu  "
                    "%u Hz x%u ch, %d frames\n",
                    static_cast<unsigned long long>(now.blocks),
                    static_cast<unsigned long long>(now.blocks - previous.blocks),
                    static_cast<unsigned long long>(now.timeouts),
                    static_cast<unsigned long long>(now.malformedBlocks),
                    static_cast<unsigned long long>(now.reclaims), now.lastSampleRate,
                    now.lastChannelCount, now.lastFrameCount);
        std::fflush(stdout);
        previous = now;
    }

    supervisor.stop();

    const ipc::ValetCounters::Snapshot final = supervisor.counters().snapshot();
    std::puts("");
    std::printf("Total blocks    : %llu\n", static_cast<unsigned long long>(final.blocks));
    std::printf("Timeouts        : %llu\n", static_cast<unsigned long long>(final.timeouts));
    std::printf("Malformed       : %llu\n", static_cast<unsigned long long>(final.malformedBlocks));
    std::printf("Reclaims        : %llu\n", static_cast<unsigned long long>(final.reclaims));
    std::printf("Format changes  : %u\n", final.formatChanges);
    std::printf("Attach cycles   : %u\n", supervisor.attachCount());

    if (!options.plugins.empty()) {
        const engine::ChainProcessor& chain = host.chainProcessor();
        std::printf("Chain blocks    : %llu\n",
                    static_cast<unsigned long long>(chain.blocksProcessed()));
        std::printf("Passed through  : %llu\n",
                    static_cast<unsigned long long>(chain.blocksPassedThrough()));
        std::printf("Format misses   : %llu\n",
                    static_cast<unsigned long long>(chain.formatMismatches()));
        std::printf("Dropped edits   : %llu\n",
                    static_cast<unsigned long long>(host.droppedParameterEdits()));
    }

    if (final.blocks == 0) {
        std::puts("");
        std::puts("No blocks were received. Either the APO is not loaded for this endpoint or no");
        std::puts("audio was playing through it.");
        return 1;
    }
    return 0;
}
