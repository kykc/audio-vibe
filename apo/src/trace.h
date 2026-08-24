// Control-plane tracing, for the one part of this system that cannot be attached to, printed
// from, or single-stepped without ceremony: a DLL inside `audiodg.exe`.
//
// design_doc.md sec. 9.5 lists "structured logging from inside audiodg.exe" as a significant
// debuggability win and files it under CAPX, contingent on the OS floor. This is the poor
// relation of that, available now: two sinks, both off by default, both selected by the `Trace`
// DWORD in the settings key (`settings.h`).
//
//   Trace = 1   OutputDebugStringW -- DebugView, or an attached debugger
//   Trace = 2   a log file, for when there is no debugger and no interactive session
//   Trace = 3   both
//
// The file sink is not a luxury. The first time this APO was loaded by a real `audiodg.exe` it
// did nothing at all, and there was no way to tell whether the engine had failed to instantiate
// it, failed to initialise it, or initialised it and then failed to open the shared stream --
// three completely different bugs with one identical symptom. `OutputDebugString` did not help,
// because catching it needs a debugger attached to a protected process. A file did.
//
// **Control thread only.** I/O of any kind is forbidden on the audio thread (sec. 7.4.1), and
// that emphatically includes both sinks. There is deliberately no variant that is safe there --
// what the audio thread has instead is counters (`BufferKing::blockCount`).
//
// The path is fixed at `C:\Windows\Temp`, which is writable by LOCAL SERVICE -- the account
// `audiodg.exe` runs as. Anywhere under a user profile would not be.

#pragma once

#include <windows.h>

#include <cstdio>

namespace aip::apo {

/// Bit 0: OutputDebugString. Bit 1: the file. Set once, from `Initialize`, before anything worth
/// tracing has happened.
inline int gTraceSinks = 0;

inline constexpr const wchar_t* kTraceFilePath = L"C:\\Windows\\Temp\\aip_apo.log";

namespace detail {

/// Appends one line. Opened and closed per line on purpose: a crash inside `audiodg.exe` must not
/// cost the diagnostic that explains it, and this is not on a hot path -- a handful of lines per
/// stream, all on the control thread.
inline void writeTraceFile(const wchar_t* line) noexcept {
    const HANDLE file = ::CreateFileW(kTraceFilePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char utf8[1024];
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (bytes > 1) {
        DWORD written = 0;
        // bytes - 1: drop the terminator, keep the newline the caller supplied.
        ::WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
    ::CloseHandle(file);
}

} // namespace detail

/// Prefixed with the process id so a DebugView session watching the whole machine, or a log file
/// surviving several `audiodg.exe` lifetimes, can still be read.
template <typename... Args>
void trace(const wchar_t* format, Args... args) {
    if (gTraceSinks == 0) {
        return;
    }
    wchar_t line[768];
    wchar_t body[640];
    if (_snwprintf_s(body, _TRUNCATE, format, args...) < 0) {
        return;
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    if (_snwprintf_s(line, _TRUNCATE, L"[%02u:%02u:%02u.%03u aip_apo %lu] %s\n", now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds, ::GetCurrentProcessId(), body) < 0) {
        return;
    }

    if ((gTraceSinks & 1) != 0) {
        ::OutputDebugStringW(line);
    }
    if ((gTraceSinks & 2) != 0) {
        detail::writeTraceFile(line);
    }
}

} // namespace aip::apo
