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
//   valet_probe --inspect       load and prepare the plugins, report, and exit; no APO involved
//   valet_probe --scan          probe every installed plugin out of process, report, and exit
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
#include "aip/rt/realtime_guard.h"
#include "aip/scanner/scanner.h"

#include <windows.h>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

#include <atomic>
#include <chrono>
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

/// A VST3 String128 is char16_t[128]; on Windows wchar_t is also 16-bit, which is the whole
/// reason the SDK offers `wscast`. Printing through it keeps this file free of a string library.
const wchar_t* asWide(const Steinberg::Vst::TChar* text) {
    return reinterpret_cast<const wchar_t*>(text);
}

struct Options {
    bool list = false;
    bool inspect = false;
    bool scan = false;
    int endpointIndex = -1; // -1 means "the default endpoint"
    float gain = 1.0f;
    int seconds = 0; // 0 means "until Ctrl+C"
    std::vector<std::string> plugins;
};

const char* kUsage =
    "Usage: valet_probe [--list] [--endpoint N] [--gain G] [--plugin PATH]... [--inspect]"
    " [--scan] [--seconds S]";

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
        } else if (std::strcmp(arg, "--inspect") == 0) {
            out.inspect = true;
        } else if (std::strcmp(arg, "--scan") == 0) {
            out.scan = true;
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

/// Reports what the host sees in each `--plugin`, then tries to prepare it. Deliberately never
/// attaches: a plugin that faults during `create` or `prepare` should cost a console process,
/// not a valet thread that `audiodg.exe` is blocked on for 1000 ms (sec. 3.7.1).
///
/// It drives PluginInstance directly rather than going through Engine, because the interesting
/// state is the bus layout *before* the arrangement is negotiated -- which a chain build would
/// have already destroyed by the time it failed. This is also the shape `scanner/` needs
/// (sec. 7.2), one process further out.
void reportBusses(Steinberg::Vst::IComponent& component,
                  Steinberg::Vst::IAudioProcessor* processor) {
    for (Steinberg::Vst::BusDirection dir : {Steinberg::Vst::kInput, Steinberg::Vst::kOutput}) {
        const Steinberg::int32 count = component.getBusCount(Steinberg::Vst::kAudio, dir);
        std::printf("       %s : %d\n",
                    dir == Steinberg::Vst::kInput ? "audio in  " : "audio out ", count);
        for (Steinberg::int32 b = 0; b < count; ++b) {
            Steinberg::Vst::BusInfo bus{};
            if (component.getBusInfo(Steinberg::Vst::kAudio, dir, b, bus) !=
                Steinberg::kResultOk) {
                continue;
            }
            Steinberg::Vst::SpeakerArrangement arrangement = 0;
            const bool haveArrangement =
                processor != nullptr &&
                processor->getBusArrangement(dir, b, arrangement) == Steinberg::kResultOk;
            std::printf("         [%d] %ls  %d ch  %s  %s  arr=0x%llx%s\n", b, asWide(bus.name),
                        bus.channelCount,
                        bus.busType == Steinberg::Vst::kMain ? "main" : "aux ",
                        (bus.flags & Steinberg::Vst::BusInfo::kDefaultActive) != 0
                            ? "default-active  "
                            : "default-inactive",
                        static_cast<unsigned long long>(arrangement),
                        haveArrangement ? "" : " (unavailable)");
        }
    }
    const Steinberg::int32 eventIn = component.getBusCount(Steinberg::Vst::kEvent,
                                                           Steinberg::Vst::kInput);
    const Steinberg::int32 eventOut = component.getBusCount(Steinberg::Vst::kEvent,
                                                            Steinberg::Vst::kOutput);
    std::printf("       event bus : %d in, %d out\n", eventIn, eventOut);
}

int runInspection(const Options& options) {
    if (options.plugins.empty()) {
        std::puts("--inspect needs at least one --plugin.");
        return 2;
    }

    Steinberg::IPtr<Steinberg::Vst::HostApplication> hostContext =
        Steinberg::owned(new Steinberg::Vst::HostApplication());

    // Nominal geometry. Nothing has told us the endpoint's real format -- protocol v1 announces
    // it nowhere (sec. 4.5) -- and for an inspection any plausible one will do.
    const engine::StreamFormat format{48000, 2, engine::kDefaultMaxFrames};

    int failures = 0;

    for (const std::string& path : options.plugins) {
        std::string error;
        std::printf("Module   : %s\n", path.c_str());

        engine::PluginModule::Ptr module = engine::PluginModule::load(path, error);
        if (!module) {
            std::printf("  FAILED : %s\n\n", error.c_str());
            ++failures;
            continue;
        }
        module->setHostContext(hostContext);
        std::printf("  name   : %s\n", module->name().c_str());

        for (const engine::PluginClass& info : module->audioEffects()) {
            std::printf("  effect : %s  [%s %s]  %s\n", info.name.c_str(), info.vendor.c_str(),
                        info.version.c_str(), info.subCategories.c_str());

            std::unique_ptr<engine::PluginInstance> instance =
                engine::PluginInstance::create(module, info.id, hostContext, error);
            if (!instance) {
                std::printf("       FAILED to instantiate: %s\n", error.c_str());
                ++failures;
                continue;
            }

            Steinberg::Vst::IComponent* component = instance->component();
            Steinberg::Vst::IEditController* controller = instance->controller();
            auto processor = Steinberg::U::cast<Steinberg::Vst::IAudioProcessor>(component);

            // A single-component plugin is one object wearing both interfaces. The distinction
            // decides how it is connected and torn down, so it is worth reporting.
            const bool single =
                Steinberg::U::cast<Steinberg::Vst::IEditController>(component) != nullptr;

            std::printf("       controller : %s\n",
                        controller == nullptr  ? "none"
                        : single               ? "same object (single component)"
                                               : "separate object (split component)");
            std::printf("       parameters : %d\n",
                        controller != nullptr ? controller->getParameterCount() : 0);
            std::printf("       latency    : %u samples\n",
                        processor ? processor->getLatencySamples() : 0u);
            std::puts("       -- as instantiated --");
            reportBusses(*component, processor);

            if (!instance->prepare(format, error)) {
                std::printf("       PREPARE FAILED at %u Hz x%u ch: %s\n", format.sampleRate,
                            format.channelCount, error.c_str());
                ++failures;
            } else {
                std::printf("       prepared at %u Hz x%u ch, up to %d frames  (%s)\n",
                            format.sampleRate, format.channelCount, format.maxFrames,
                            instance->fullBusNegotiation()
                                ? "all busses negotiated"
                                : "main busses only; aux busses left connected");
                std::puts("       -- as prepared --");
                reportBusses(*component, processor);
            }
        }
        std::puts("");
    }

    if (failures != 0) {
        std::printf("%d failure(s). Nothing was attached; no audio was touched.\n", failures);
        return 1;
    }
    std::puts("All plugins prepared. Nothing was attached; no audio was touched.");
    return 0;
}

/// Probes every installed plugin the way the shell will: out of process, one entry per bundle,
/// and a plugin that faults costs its own entry and nothing else (sec. 7.2).
///
/// The difference from `--inspect` is the whole point of `scanner/`. `--inspect` does the same
/// probing *in this process*, which is fine for a plugin already known to be sound and fatal for
/// one that is not -- and "which of my plugins is not" is exactly the question a scan answers.
int runScan(const Options& options) {
    const std::vector<std::string> paths = options.plugins.empty()
                                               ? engine::PluginModule::installedModulePaths()
                                               : options.plugins;
    if (paths.empty()) {
        std::puts("No VST3 plugins found in the standard search locations.");
        return 0;
    }
    std::printf("Scanning %zu bundle(s) out of process.\n\n", paths.size());

    const auto started = std::chrono::steady_clock::now();
    const scanner::ScanReport report = scanner::scanModules(
        paths, scanner::ScanOptions{},
        [](const scanner::ScannedModule& module, std::size_t done, std::size_t total) {
            std::printf("[%3zu/%3zu] %-11s %s\n", done, total, scanner::toString(module.status),
                        module.path.c_str());
            for (const scanner::ScannedClass& info : module.classes) {
                std::printf("            %s  [%s %s]  %d params, %d in / %d out%s%s\n",
                            info.name.c_str(), info.vendor.c_str(), info.version.c_str(),
                            info.parameterCount, info.mainInputChannels, info.mainOutputChannels,
                            info.hasEditor ? ", editor" : "",
                            info.prepared ? "" : ", NOT PREPARED");
                if (!info.error.empty()) {
                    std::printf("            -- %s\n", info.error.c_str());
                }
            }
            if (!module.error.empty()) {
                std::printf("            -- %s\n", module.error.c_str());
            }
        });

    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    std::printf("\n%zu usable, %zu unloadable, %zu crashed, %zu timed out"
                " -- %d scanner process(es), %.1f s\n",
                report.countWith(scanner::ScanStatus::Ok),
                report.countWith(scanner::ScanStatus::LoadFailed),
                report.countWith(scanner::ScanStatus::Crashed),
                report.countWith(scanner::ScanStatus::TimedOut), report.childProcesses,
                seconds.count());
    std::puts("Nothing was attached; no audio was touched.");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    // COM for MMDevice enumeration, and for whatever a plugin does during activation. Control
    // thread only -- COM activation is forbidden on the audio thread (sec. 7.4.1).
    ipc::ComApartment com;
    if (!com.ok()) {
        std::puts("Failed to initialise COM.");
        return 1;
    }

    if (options.inspect) {
        return runInspection(options);
    }

    if (options.scan) {
        return runScan(options);
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

    // The real-time violation detector is linked into this executable on purpose (sec. 7.4.6):
    // a run against the real APO is the only place our audio path is exercised by the real
    // producer, so it is the most valuable reading we can take. Zeroed here so the count covers
    // the run and not the process start-up.
    rt::resetViolations();

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

    if constexpr (rt::checksEnabled()) {
        const rt::ViolationCounts counts = rt::violations();
        std::puts("");
        std::printf("Audio-thread allocations : %llu\n",
                    static_cast<unsigned long long>(counts.allocations));
        std::printf("Audio-thread frees       : %llu\n",
                    static_cast<unsigned long long>(counts.deallocations));
        std::printf("Audio-thread locks       : %llu\n",
                    static_cast<unsigned long long>(counts.locks));
        if (counts.total() != 0 && !options.plugins.empty()) {
            std::puts("Note: the detector counts everything on the valet thread, the plugin's own");
            std::puts("work included. A nonzero count here is not by itself a defect in our code");
            std::puts("(sec. 7.4.5) -- rerun without --plugin to see whether the plumbing is clean.");
        }
        std::puts("");
    }

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
