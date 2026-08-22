// Surviving a plugin that kills the shell while audio is running through it.
//
// `load_guard.h` covers the half of this that has a call to bracket: a plugin that faults inside
// `initialize` does it between two statements, so writing the path down before and removing it
// after is enough to name the culprit at the next start. A plugin that faults inside `process`
// gives nothing to bracket. It happens on the audio thread, seconds or hours after the rack was
// built, and because being attached is restored too (session.h) the next start walks into it
// again -- and every attempt takes the machine's audio with it for as long as it lasts.
//
// Two answers were possible. Time the attach and treat "it survived N seconds" as proof, or do
// not restore the attach at all when the last run ended badly. The project owner chose the second
// on 2026-08-22: the first needs a threshold that is a guess about how quickly a bad plugin
// faults, and a wrong guess either protects nothing or withholds protection from a slow crash.
// So there is no threshold here. The shell comes up detached, says why, and waits to be asked.
//
// The mechanism is a mark rather than a breadcrumb: written when the shell attaches, removed when
// it detaches or exits. Found at the next start, it means the previous run was attached to an
// endpoint at the moment it stopped existing.
//
// What that must not do is cry wolf. An application that announces a crash after an ordinary
// Windows restart is a familiar and thoroughly untrustworthy thing -- the project owner has been
// on the receiving end of it (2026-08-22) -- and the shape of the bug is always the same: the
// mark is only cleared on the application's own quit path, and a shutdown does not go through it.
// Windows asks first, though. Every top-level window gets `WM_QUERYENDSESSION` before the session
// ends, and processes that are killed for being slow are killed after it. So the shell clears the
// mark from that message as well as from its own exit (ui/src/session_end_filter.h), which is
// what makes a reboot with the shell left open an ordinary end rather than an accusation.
//
// Two ends are outside that: losing power, and being force-killed from Task Manager. Both leave
// the mark behind and both are reported as an unclean end, which for the first is arguably true
// -- the shell really was processing audio when it stopped -- and costs one press of Attach.

#pragma once

#include <filesystem>
#include <string>

namespace aip::config {

/// What the previous run left behind if it was attached when it stopped existing.
struct UncleanAttach {
    /// False is the ordinary case: the last run either was not attached or ended tidily.
    bool present = false;
    /// The endpoint it was processing, so the shell can say which one in words a user
    /// recognises. May be empty -- the mark is worth acting on either way.
    std::string endpointName;
};

class AttachGuard {
public:
    /// `sessionPath` is the session file this shell is using; the mark is its sibling. An empty
    /// path makes every operation a no-op.
    explicit AttachGuard(const std::filesystem::path& sessionPath);

    ~AttachGuard();

    AttachGuard(const AttachGuard&) = delete;
    AttachGuard& operator=(const AttachGuard&) = delete;

    /// Where the mark lives. Named for what it is, because a user who finds it should be able to
    /// guess what it does and that deleting it is safe.
    [[nodiscard]] static std::filesystem::path markPath(const std::filesystem::path& sessionPath);

    /// Whether the previous run was attached when it ended, and to what. Call before `mark`,
    /// which overwrites it.
    ///
    /// It takes rather than reads, for the same reason as `LoadGuard::takePreviousCasualty`: a
    /// mark that outlived being acted on would suppress every future reattach, and no amount of
    /// attaching by hand would clear it.
    [[nodiscard]] static UncleanAttach takePrevious(const std::filesystem::path& sessionPath);

    /// Records that the shell is attached to `endpointName`, and flushes -- the data has to reach
    /// the operating system now, because the process that would flush it later is the one this is
    /// defending against. Marking again with the same endpoint costs nothing, so this is safe to
    /// call from wherever the attached state is noticed rather than only where it changes.
    void mark(const std::string& endpointName);

    /// Records that the shell is no longer attached, or is on its way out. Idempotent.
    void clear();

    [[nodiscard]] bool marked() const noexcept { return marked_; }

private:
    std::filesystem::path path_;
    bool marked_ = false;
    std::string endpointName_;
};

} // namespace aip::config
