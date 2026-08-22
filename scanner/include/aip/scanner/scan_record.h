// The wire format between a scanner child and its parent.
//
// One record per line, `key` then a space then the value, on the child's stdout. Two properties
// decide the design, and nothing else about it is interesting:
//
//   1. **It must survive the writer dying mid-stream.** The whole point of the child process is
//      that it may fault at any instant (sec. 7.2). A format needing a closing bracket to be
//      parseable -- JSON, for one -- would make the records already safely received unreadable
//      because of the plugin that came after them. Line records are complete the moment their
//      newline lands, and the parent flushes its understanding as they arrive.
//   2. **It must stay ASCII whatever a plugin is called.** Plugin names, vendors and paths are
//      third-party text and are under no obligation to be ASCII or even valid UTF-8. Values are
//      therefore escaped: `\\` for a backslash and `\xHH` for every byte outside 0x20-0x7E, which
//      also disposes of the newline that would otherwise end the record early. Sec. 6.6 governs
//      this repository's own files, but a transport that only works for well-behaved input is a
//      transport that fails on exactly the plugins a scanner exists to survive.
//
// The parser ignores keys it does not know, so a newer child can be read by an older parent
// without either being taught about the other.

#pragma once

#include "aip/scanner/scan_result.h"

#include <string>
#include <vector>

namespace aip::scanner {

/// Escapes `raw` so that it occupies exactly one line of printable ASCII.
[[nodiscard]] std::string escapeField(const std::string& raw);

/// Reverses `escapeField`. Malformed escapes are passed through literally rather than rejected:
/// a diagnostic that arrives slightly mangled beats one thrown away on the way to the user.
[[nodiscard]] std::string unescapeField(const std::string& text);

/// Announces that the child is about to enter third-party code for `path`. Written and *flushed*
/// before the probe starts, never after: it is the only thing that tells a parent reading the
/// wreckage which plugin was on the table. Emitting it afterwards would make every crash
/// anonymous.
[[nodiscard]] std::string encodeModuleBegin(const std::string& path);

/// Everything the probe learned, terminated by the `end` record. Written once the probe returns.
[[nodiscard]] std::string encodeModuleBody(const ScannedModule& module);

/// Both halves, for a caller with no crash to worry about -- tests, and anything replaying a
/// stored report.
[[nodiscard]] std::string encodeModule(const ScannedModule& module);

/// Assembles modules from the child's line stream.
///
/// The reader is the parent's only source of truth about what the child was doing when it died:
/// `inFlight()` is true from the `begin` record until the matching `end`, and it names the module
/// to blame. That is the mechanism by which a crash costs one entry instead of a scan.
class RecordReader {
public:
    /// Feeds one line, without its newline. Returns true when the line completed a module, which
    /// is then available from `release()`.
    bool consumeLine(const std::string& line);

    /// The module the last `consumeLine` completed. Only valid immediately after it returned true.
    [[nodiscard]] ScannedModule release();

    /// True while a module has been begun and not ended -- i.e. the child is inside third-party
    /// code for it right now.
    [[nodiscard]] bool inFlight() const noexcept { return inFlight_; }

    [[nodiscard]] const std::string& inFlightPath() const noexcept { return pending_.path; }

    /// Gives up on the in-flight module, stamping it with why. Clears the in-flight state so the
    /// reader can be reused for the next child.
    [[nodiscard]] ScannedModule abandon(ScanStatus status, const std::string& error);

private:
    ScannedModule pending_;
    ScannedModule completed_;
    bool inFlight_ = false;
};

} // namespace aip::scanner
