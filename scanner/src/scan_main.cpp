// aip_scan -- the short-lived child process that probes plugins (design_doc.md sec. 7.2).
//
//   aip_scan <path>...            probe these bundles, write records to stdout
//   aip_scan                      read the bundle list from stdin, one escaped path per line
//   aip_scan --report-handle N    write records to inherited handle N instead of stdout
//   aip_scan --no-prepare         load and instantiate, but do not activate
//   aip_scan --no-editor          do not ask classes for an editor view
//   aip_scan --rate N / --channels N   the format classes are asked to prepare for
//
// Run it by hand on a suspect plugin and it prints its own wire format, which is legible enough
// to read directly. That is deliberate: the alternative -- a binary or handle-only protocol --
// would make the one tool built for diagnosing a hostile plugin undiagnosable itself.
//
// Two things about this program are load-bearing and easy to undo by accident:
//
//   * **Records go to a handle, not to stdio.** Plugins print. A plugin that writes a banner to
//     stdout during `initialize` would land in the middle of a record and corrupt the entry
//     either side of it, so the parent hands over a private pipe and lets the plugin have stdout
//     to itself. Handles also mean no CRT buffer to flush and none to lose in a fault.
//   * **`begin` is written before the probe, and the write is not buffered.** The parent's only
//     way to attribute a crash is the last `begin` it saw.

#include "aip/ipc/endpoints.h"
#include "aip/scanner/probe_worker.h"
#include "aip/scanner/scan_record.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace aip;

namespace {

const char* kUsage =
    "Usage: aip_scan [--report-handle N] [--no-prepare] [--no-editor] [--rate N]"
    " [--channels N] [<path>...]";

struct Options {
    scanner::ProbeOptions probe;
    HANDLE report = INVALID_HANDLE_VALUE;
    std::vector<std::string> paths;
};

bool parseOptions(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool hasValue = i + 1 < argc;

        if (std::strcmp(arg, "--report-handle") == 0 && hasValue) {
            const auto value = static_cast<std::uintptr_t>(std::strtoull(argv[++i], nullptr, 10));
            out.report = reinterpret_cast<HANDLE>(value);
        } else if (std::strcmp(arg, "--no-prepare") == 0) {
            out.probe.prepare = false;
        } else if (std::strcmp(arg, "--no-editor") == 0) {
            out.probe.queryEditor = false;
        } else if (std::strcmp(arg, "--rate") == 0 && hasValue) {
            out.probe.sampleRate = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(arg, "--channels") == 0 && hasValue) {
            out.probe.channelCount = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        } else if (arg[0] == '-') {
            std::printf("Unrecognised argument: %s\n%s\n", arg, kUsage);
            return false;
        } else {
            out.paths.emplace_back(arg);
        }
    }
    return true;
}

/// Blocking write of the whole buffer. A short write on a pipe is normal when the parent is slow;
/// treating one as done would silently truncate a record.
bool writeAll(HANDLE handle, const std::string& text) {
    const char* data = text.data();
    DWORD remaining = static_cast<DWORD>(text.size());
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, data, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        remaining -= written;
    }
    return true;
}

/// The work list, when it did not come in on the command line. One escaped path per line, ended
/// by the parent closing its end -- there is no count and no terminator record, because a parent
/// that dies half way through should leave the child with a short list rather than a hang.
std::vector<std::string> readWorkList() {
    std::vector<std::string> paths;
    std::string line;
    for (int ch = std::getchar(); ch != EOF; ch = std::getchar()) {
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                paths.push_back(scanner::unescapeField(line));
            }
            line.clear();
            continue;
        }
        line += static_cast<char>(ch);
    }
    if (!line.empty()) {
        paths.push_back(scanner::unescapeField(line));
    }
    return paths;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 2;
    }

    // Before any plugin code runs, and before COM: a fault during CoInitializeEx is as capable of
    // raising a dialog as one inside a plugin.
    scanner::suppressCrashDialogs();

    if (options.report == INVALID_HANDLE_VALUE) {
        options.report = GetStdHandle(STD_OUTPUT_HANDLE);
    }

    // Whatever a plugin does during activation may need COM. Control thread only -- COM
    // activation is forbidden on the audio thread (sec. 7.4.1), and there is no audio thread here.
    const ipc::ComApartment com;
    if (!com.ok()) {
        std::puts("Failed to initialise COM.");
        return 2;
    }

    if (options.paths.empty()) {
        options.paths = readWorkList();
    }

    for (const std::string& path : options.paths) {
        if (!writeAll(options.report, scanner::encodeModuleBegin(path))) {
            return 3; // the parent is gone; there is no one left to report to
        }
        const scanner::ScannedModule module = scanner::probeModule(path, options.probe);
        if (!writeAll(options.report, scanner::encodeModuleBody(module))) {
            return 3;
        }
    }
    return 0;
}
