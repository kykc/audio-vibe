// Surviving a plugin that kills the shell while the session is being restored.
//
// The rack is rebuilt in this process at startup, from whatever the session file names, and
// building it means loading DLLs and calling `initialize` on third-party code. A plugin that
// faults there takes the shell down *before it draws* -- and it will do it again on the next
// start, and the next, because nothing has changed. This machine has three plugins that would
// (status.md sec. 4), and since the session started naming plugins automatically, and since being
// attached is restored too, a start that crashes now also takes the machine's audio with it on
// the way there. That is an unusable application recoverable only by editing a file the user does
// not know exists.
//
// `scanner/` is the thorough answer and the wrong one *here*: it costs a child process per rack
// entry at every start, and the catalog already knows the answer for anything it has scanned. So
// the catalog is consulted first, and this is what covers the rest -- a breadcrumb.
//
// Write down what is about to be loaded. Clear it when the load returns. If it is still there at
// the next start, the thing it names is what stopped the previous one from finishing, and it is
// skipped rather than tried again. One file, no processes, and it turns an unstartable shell into
// a shell that starts with one plugin missing and a line saying which and why.
//
// The one thing it cannot do is survive losing power: the breadcrumb is flushed to the operating
// system, which is enough for a process that faults or is killed -- the data is in the file cache
// and outlives the process -- and not enough for a machine that stops. That is the right trade;
// the failure being defended against is a plugin, not a power cut.

#pragma once

#include <filesystem>
#include <string>

namespace aip::config {

class LoadGuard {
public:
    /// `sessionPath` is the file being restored; the breadcrumb is its sibling. An empty path
    /// makes every operation a no-op, which is what a session that was never loaded wants.
    explicit LoadGuard(const std::filesystem::path& sessionPath);

    ~LoadGuard();

    LoadGuard(const LoadGuard&) = delete;
    LoadGuard& operator=(const LoadGuard&) = delete;

    /// Where the breadcrumb lives. Named for what it is, because a user who finds it should be
    /// able to guess what it does and that deleting it is safe.
    [[nodiscard]] static std::filesystem::path breadcrumbPath(
        const std::filesystem::path& sessionPath);

    /// What the previous run was in the middle of loading when it stopped, or empty if it got
    /// through. Call this *before* anything is loaded -- `mark` overwrites it.
    ///
    /// It takes rather than reads: the breadcrumb is removed on the way out. A breadcrumb that
    /// outlived being acted on would be permanent, and the entry it names could never be tried
    /// again -- clearing `blocked` by hand would be undone by the same stale file on the next
    /// start, with no way to tell that is what was happening.
    [[nodiscard]] static std::string takePreviousCasualty(
        const std::filesystem::path& sessionPath);

    /// Records that `path` is about to be loaded, and flushes. Call immediately before the load.
    void mark(const std::string& path);

    /// Records that the load returned. Call immediately after, whether it succeeded or not: a
    /// plugin that fails cleanly is not the one this is looking for.
    void clear();

private:
    std::filesystem::path path_;
    bool marked_ = false;
};

} // namespace aip::config
