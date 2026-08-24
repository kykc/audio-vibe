// The half of the scanner that runs in the *parent* process (design_doc.md sec. 7.2).
//
// Nothing here loads a plugin. That is the entire value proposition: `scanModules` starts a child,
// feeds it a work list, and reads back what it learned -- so a plugin that faults on load costs one
// entry in the report rather than the process that was going to host the user's session.
//
// The process shape follows sec. 7.2's "one short-lived scanner process per scan" and keeps
// per-plugin isolation anyway, by making the child resumable. The child announces each bundle
// before it touches it; if the child then dies, the parent knows exactly which entry to blame,
// marks it, and relaunches for the remainder of the list. A clean machine therefore costs one
// process for the whole scan, and each bad plugin costs one more.
//
// `scanModules` blocks for as long as the scan takes -- minutes, on a machine with a lot of
// plugins -- so a UI must run it on a worker thread. It is written for that: `progress` is called
// as each entry lands, and `cancelled` is polled between entries.

#pragma once

#include "aip/scanner/probe_worker.h"
#include "aip/scanner/scan_result.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace aip::scanner {

struct ScanOptions {
    ProbeOptions probe;

    /// How long one bundle may occupy the child before it is presumed stuck and terminated.
    ///
    /// Generous on purpose. A plugin that loads a neural model or scans its own preset library can
    /// legitimately take many seconds on a cold file cache, and a scan that declares a working
    /// plugin broken is worse than a scan that takes a while: the user sees a plugin they own
    /// greyed out, with no way to tell that patience would have fixed it.
    unsigned moduleTimeoutMs = 60000;

    /// Path to `aip_scan.exe`. Empty means "next to the running executable", which is where an
    /// installed layout puts it.
    std::string childExecutable;

    /// Polled between entries. Null means the scan cannot be cancelled.
    const std::atomic<bool>* cancelled = nullptr;
};

/// Called as each entry completes, with how many of `paths` are now accounted for. Runs on
/// whatever thread called `scanModules` -- a Qt caller must marshal, not paint from here.
using ScanProgress = std::function<void(const ScannedModule& module, std::size_t done, std::size_t total)>;

/// Probes every path, in order, and returns one entry per path.
///
/// Never throws and has no failure return: a scan that cannot start a child still produces a
/// report, with every entry marked and the reason in its `error`. There is no sensible way for a
/// caller to react differently, and a report is the thing the caller needs either way.
[[nodiscard]] ScanReport scanModules(
    const std::vector<std::string>& paths, const ScanOptions& options = {}, const ScanProgress& progress = {});

/// Where `scanModules` would look for the child, and why it did not find it. Exposed for the
/// diagnostic it gives: "the scanner is missing" is otherwise indistinguishable from "every one of
/// your plugins is broken", because both produce a report full of failures.
[[nodiscard]] std::string locateChildExecutable(std::string& error);

} // namespace aip::scanner
