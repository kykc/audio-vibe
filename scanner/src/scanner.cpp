#include "aip/scanner/scanner.h"

#include "aip/scanner/scan_record.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aip::scanner {

namespace {

/// Paths in this project are UTF-8 `std::string` throughout, because that is what the SDK's
/// `Module::getModulePaths()` hands out. Everything the Win32 API needs is UTF-16.
[[nodiscard]] std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

[[nodiscard]] std::string narrow(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

[[nodiscard]] bool fileExists(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/// `GetModuleFileNameW` truncates rather than failing on a small buffer, and MAX_PATH is not a
/// bound this project may assume (sec. 6.3.1). Grow until it fits.
[[nodiscard]] std::wstring ownExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (written == 0) {
            return {};
        }
        if (written < path.size()) {
            path.resize(written);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

[[nodiscard]] std::wstring directoryOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"/\\");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

/// Closes on scope exit. There is no shortage of these in the Win32 world and the project has no
/// general one; this is deliberately the smallest thing that serves this file.
class Handle {
public:
    Handle() = default;

    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}

    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    [[nodiscard]] HANDLE* address() noexcept { return &handle_; }

private:
    HANDLE handle_ = nullptr;
};

enum class Wait {
    Line, ///< a record line is available
    Timeout, ///< nothing arrived within the deadline; the child is presumed stuck
    Ended, ///< the pipe closed, which means the child is gone one way or another
};

/// One `aip_scan` process, its private record pipe, and the thread that drains it.
///
/// The reader has to be a thread rather than a poll loop because the two events that end a wait --
/// a line arriving and the child dying -- are a pipe read and a process exit, and only a blocking
/// read reports the second promptly. Nothing here is on an audio path, so a thread and a
/// condition variable are free.
class ChildProcess {
public:
    ~ChildProcess() { stop(); }

    [[nodiscard]] bool start(const std::wstring& executable, const ScanOptions& options, std::string& error) {
        SECURITY_ATTRIBUTES inheritable{};
        inheritable.nLength = sizeof inheritable;
        inheritable.bInheritHandle = TRUE;

        Handle reportWrite;
        // A megabyte of pipe so the child never blocks on a parent that is briefly busy. Records
        // are small; this is about a hundred plugins' worth.
        if (!CreatePipe(reportRead_.address(), reportWrite.address(), &inheritable, 1 << 20)) {
            error = "failed to create the report pipe";
            return false;
        }
        // Our end must not be inherited, or the child holds a copy of the read handle and the
        // pipe never reports EOF when the child dies -- which is exactly the event we need.
        SetHandleInformation(reportRead_.get(), HANDLE_FLAG_INHERIT, 0);

        Handle workListRead;
        if (!CreatePipe(workListRead.address(), workList_.address(), &inheritable, 1 << 20)) {
            error = "failed to create the work list pipe";
            return false;
        }
        SetHandleInformation(workList_.get(), HANDLE_FLAG_INHERIT, 0);

        // Plugins print. Handing the child a null device for stdout and stderr keeps a plugin's
        // banner out of the parent's console, and -- because records travel on their own pipe --
        // out of the record stream that a banner would otherwise corrupt.
        Handle nul(CreateFileW(
            L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr));

        HANDLE inherited[3] = {workListRead.get(), reportWrite.get(), nul.get()};
        const DWORD inheritedCount = nul.valid() ? 3u : 2u;

        // An explicit handle list rather than "inherit everything inheritable". The shell holds
        // shared-memory and event handles for a live valet (sec. 4.2); a scanner child has no
        // business holding a duplicate of any of them, least of all one that survives in a
        // process about to be killed for hanging.
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<unsigned char> attributeStorage(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)) {
            error = "failed to build the child's handle list";
            return false;
        }
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                inheritedCount * sizeof(HANDLE), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attributes);
            error = "failed to build the child's handle list";
            return false;
        }

        std::wstring command = L"\"" + executable + L"\" --report-handle " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(reportWrite.get()));
        command += L" --rate " + std::to_wstring(options.probe.sampleRate);
        command += L" --channels " + std::to_wstring(options.probe.channelCount);
        if (!options.probe.prepare) {
            command += L" --no-prepare";
        }
        if (!options.probe.queryEditor) {
            command += L" --no-editor";
        }
        std::vector<wchar_t> commandBuffer(command.begin(), command.end());
        commandBuffer.push_back(L'\0');

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof startup;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = workListRead.get();
        startup.StartupInfo.hStdOutput = nul.get();
        startup.StartupInfo.hStdError = nul.get();
        startup.lpAttributeList = attributes;

        // The child gets the temp directory as its working directory, not the caller's.
        //
        // Plugins write files. At least one on this machine drops a `device_info.txt` into the
        // current directory the moment it is loaded, and since a scan loads *every* plugin on the
        // machine, whatever directory the shell happens to have been started in collects the
        // litter of all of them. It found this project by scanning from the repository root and
        // breaking the sec. 6.6 ASCII gate with a binary file. Nothing legitimate resolves against
        // the current directory here -- a plugin finds its own resources through its module path,
        // which the SDK's loader supplies -- so pointing it at temp costs nothing and contains it.
        wchar_t tempDirectory[MAX_PATH + 1] = {};
        const DWORD tempLength = GetTempPathW(MAX_PATH + 1, tempDirectory);
        const wchar_t* workingDirectory = (tempLength > 0 && tempLength <= MAX_PATH) ? tempDirectory : nullptr;

        PROCESS_INFORMATION info{};
        const BOOL started = CreateProcessW(executable.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, workingDirectory, &startup.StartupInfo, &info);
        const DWORD launchError = GetLastError();
        DeleteProcThreadAttributeList(attributes);

        if (!started) {
            error = "failed to start " + narrow(executable) + " (error " + std::to_string(launchError) + ")";
            return false;
        }
        process_.reset(info.hProcess);
        CloseHandle(info.hThread);

        // The parent's copies of the child's ends go now. Keeping the write end alive here would
        // hold the pipe open after the child died and turn every crash into a timeout.
        reportWrite.reset();
        workListRead.reset();
        nul.reset();

        reader_ = std::thread([this] { readLoop(); });
        return true;
    }

    /// Hands over the bundles to probe and closes the pipe, which is what tells the child the
    /// list is complete.
    void sendWorkList(const std::vector<std::string>& paths) {
        std::string payload;
        for (const std::string& path : paths) {
            payload += escapeField(path);
            payload += '\n';
        }
        const char* data = payload.data();
        DWORD remaining = static_cast<DWORD>(payload.size());
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(workList_.get(), data, remaining, &written, nullptr) || written == 0) {
                break; // the child is already gone; the read loop will report it
            }
            data += written;
            remaining -= written;
        }
        workList_.reset();
    }

    [[nodiscard]] Wait nextLine(std::string& line, unsigned timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool arrived =
            ready_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !lines_.empty() || ended_; });
        if (!arrived) {
            return Wait::Timeout;
        }
        if (!lines_.empty()) {
            line = std::move(lines_.front());
            lines_.pop_front();
            return Wait::Line;
        }
        return Wait::Ended;
    }

    /// The child's exit code, once it has one. Reported in the crash diagnostic because it
    /// distinguishes an access violation from a plugin calling `exit`.
    [[nodiscard]] std::string exitDescription() {
        DWORD code = 0;
        if (process_.valid() && GetExitCodeProcess(process_.get(), &code) && code != STILL_ACTIVE) {
            char text[32];
            std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(code));
            return text;
        }
        return "unknown";
    }

    void kill() noexcept {
        if (process_.valid()) {
            TerminateProcess(process_.get(), 1);
        }
    }

    void stop() noexcept {
        if (process_.valid()) {
            // A child that has finished its list exits on its own; one being abandoned has not.
            if (WaitForSingleObject(process_.get(), 2000) != WAIT_OBJECT_0) {
                TerminateProcess(process_.get(), 1);
                WaitForSingleObject(process_.get(), 2000);
            }
        }
        if (reader_.joinable()) {
            reader_.join(); // the pipe reports EOF once the child's write end is gone
        }
        workList_.reset();
        reportRead_.reset();
        process_.reset();
    }

private:
    void readLoop() {
        std::string buffer;
        char chunk[4096];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(reportRead_.get(), chunk, sizeof chunk, &read, nullptr) || read == 0) {
                break;
            }
            buffer.append(chunk, read);

            std::size_t start = 0;
            std::size_t newline = buffer.find('\n', start);
            while (newline != std::string::npos) {
                std::string line = buffer.substr(start, newline - start);
                // The child writes through a handle, so no CRT translation should be adding these
                // -- but a stray carriage return would silently corrupt the last field of every
                // record, and stripping it costs nothing.
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    lines_.push_back(std::move(line));
                }
                ready_.notify_one();
                start = newline + 1;
                newline = buffer.find('\n', start);
            }
            buffer.erase(0, start);
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ended_ = true;
        }
        ready_.notify_all();
    }

    Handle process_;
    Handle reportRead_;
    Handle workList_;
    std::thread reader_;

    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
    bool ended_ = false;
};

[[nodiscard]] ScannedModule stubFor(const std::string& path, ScanStatus status, const std::string& error) {
    ScannedModule module;
    module.path = path;
    module.status = status;
    module.error = error;
    return module;
}

void report(const ScanProgress& progress, const ScannedModule& module, std::size_t done, std::size_t total) {
    if (progress) {
        progress(module, done, total);
    }
}

} // namespace

std::string locateChildExecutable(std::string& error) {
    error.clear();

    const std::wstring own = ownExecutablePath();
    const std::wstring sibling = directoryOf(own) + L"aip_scan.exe";
    if (!own.empty() && fileExists(sibling)) {
        return narrow(sibling);
    }

#ifdef AIP_SCAN_EXECUTABLE
    // The build tree puts every executable in its own target directory, so "next to me" is only
    // true once installed. The same escape hatch the tests use for the fixture plugin path.
    const std::wstring configured = widen(AIP_SCAN_EXECUTABLE);
    if (fileExists(configured)) {
        return narrow(configured);
    }
#endif

    error = "aip_scan.exe was not found next to " + narrow(own) + " -- plugins cannot be probed without it";
    return {};
}

ScanReport scanModules(
    const std::vector<std::string>& paths, const ScanOptions& options, const ScanProgress& progress) {
    ScanReport result;
    if (paths.empty()) {
        return result;
    }
    result.modules.reserve(paths.size());

    std::string error;
    const std::string executable =
        options.childExecutable.empty() ? locateChildExecutable(error) : options.childExecutable;
    if (executable.empty()) {
        for (const std::string& path : paths) {
            result.modules.push_back(stubFor(path, ScanStatus::LoadFailed, error));
            report(progress, result.modules.back(), result.modules.size(), paths.size());
        }
        return result;
    }
    const std::wstring executableW = widen(executable);

    const auto cancelled = [&options] {
        return options.cancelled != nullptr && options.cancelled->load(std::memory_order_relaxed);
    };

    while (result.modules.size() < paths.size()) {
        if (cancelled()) {
            break;
        }

        ChildProcess child;
        if (!child.start(executableW, options, error)) {
            while (result.modules.size() < paths.size()) {
                result.modules.push_back(stubFor(paths[result.modules.size()], ScanStatus::LoadFailed, error));
                report(progress, result.modules.back(), result.modules.size(), paths.size());
            }
            break;
        }
        ++result.childProcesses;
        child.sendWorkList({paths.begin() + static_cast<std::ptrdiff_t>(result.modules.size()), paths.end()});

        RecordReader reader;
        bool childEnded = false;
        bool consumedThisChild = false;

        while (result.modules.size() < paths.size() && !cancelled()) {
            std::string line;
            const Wait wait = child.nextLine(line, options.moduleTimeoutMs);

            if (wait == Wait::Timeout) {
                child.kill();
                const std::string reason =
                    "no progress for " + std::to_string(options.moduleTimeoutMs) + " ms; the child was terminated";
                result.modules.push_back(reader.inFlight()
                        ? reader.abandon(ScanStatus::TimedOut, reason)
                        : stubFor(paths[result.modules.size()], ScanStatus::TimedOut, reason));
                report(progress, result.modules.back(), result.modules.size(), paths.size());
                consumedThisChild = true;
                childEnded = true;
                break;
            }
            if (wait == Wait::Ended) {
                childEnded = true;
                break;
            }
            if (reader.consumeLine(line)) {
                result.modules.push_back(reader.release());
                report(progress, result.modules.back(), result.modules.size(), paths.size());
                consumedThisChild = true;
            }
        }

        if (childEnded && result.modules.size() < paths.size()) {
            const std::string reason =
                "the scanner process exited (code " + child.exitDescription() + ") while probing this plugin";
            if (reader.inFlight()) {
                // The child announced this bundle and never came back from it. This is the
                // precise case the whole design exists for.
                result.modules.push_back(reader.abandon(ScanStatus::Crashed, reason));
                report(progress, result.modules.back(), result.modules.size(), paths.size());
            } else if (!consumedThisChild) {
                // It died without announcing anything, so there is nobody to blame -- and the next
                // entry is charged regardless. Not arbitrary: every abnormal exit that made no
                // progress must consume an entry, or a child that dies instantly (a missing DLL,
                // a policy blocking the executable) is relaunched against an unchanged work list
                // for ever. Charging an innocent plugin is visible and recoverable; a scan that
                // never ends is neither.
                result.modules.push_back(stubFor(paths[result.modules.size()], ScanStatus::Crashed, reason));
                report(progress, result.modules.back(), result.modules.size(), paths.size());
            }
            // Otherwise it exited between entries having made progress: nothing to charge, and
            // the next child picks up the remainder.
        }
    }

    // A cancelled scan still returns one entry per path: a caller that has to special-case a
    // short vector will eventually forget to.
    while (result.modules.size() < paths.size()) {
        result.modules.push_back(
            stubFor(paths[result.modules.size()], ScanStatus::NotProbed, "the scan was cancelled"));
    }
    return result;
}

} // namespace aip::scanner
