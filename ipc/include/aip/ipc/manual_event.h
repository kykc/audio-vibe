// RAII wrapper over the two manual-reset Win32 events of protocol v1 (design_doc.md sec. 4.2).
//
// Both events are manual-reset; KING is created signaled and VALET non-signaled. `set`,
// `reset` and `wait` are the sanctioned real-time operations of sec. 7.4.4 and are therefore
// noexcept, allocation-free, and callable from the valet thread. `open` and `create` are
// control-thread only.

#pragma once

#include <windows.h>

#include <string>
#include <utility>

namespace aip::ipc {

enum class WaitResult {
    Signaled,
    Timeout,
    Failed,
};

class ManualEvent {
public:
    ManualEvent() = default;

    ~ManualEvent() { close(); }

    ManualEvent(ManualEvent&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    ManualEvent& operator=(ManualEvent&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ManualEvent(const ManualEvent&) = delete;
    ManualEvent& operator=(const ManualEvent&) = delete;

    /// Opens an existing event by name, as the valet does at attach (sec. 4.4). Returns false when
    /// the object does not exist, which means the endpoint is not currently active.
    [[nodiscard]] static bool open(const std::wstring& name, ManualEvent& out);

    /// Creates (or attaches to) a named manual-reset event with a null DACL, as the king does
    /// (sec. 4.2). Only the synthetic king of the conformance harness and the future APO need this.
    [[nodiscard]] static bool create(const std::wstring& name, bool initiallySignaled,
                                     ManualEvent& out);

    /// Creates an unnamed, process-local manual-reset event. Not part of protocol v1 -- used by
    /// the control plane (e.g. the supervisor's interruptible sleep) and never on the audio
    /// thread, where the sanctioned waits are the KING/VALET events only (sec. 7.4.4).
    [[nodiscard]] static bool createLocal(bool initiallySignaled, ManualEvent& out);

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    void set() noexcept { ::SetEvent(handle_); }

    void reset() noexcept { ::ResetEvent(handle_); }

    /// `timeoutMs` may be INFINITE. Real-time safe on the protocol events only (sec. 7.4.4).
    [[nodiscard]] WaitResult wait(DWORD timeoutMs) noexcept {
        switch (::WaitForSingleObject(handle_, timeoutMs)) {
        case WAIT_OBJECT_0:
            return WaitResult::Signaled;
        case WAIT_TIMEOUT:
            return WaitResult::Timeout;
        default:
            return WaitResult::Failed;
        }
    }

    void close() noexcept {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

private:
    HANDLE handle_ = nullptr;
};

} // namespace aip::ipc
