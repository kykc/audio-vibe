#include "aip/ipc/manual_event.h"

#include "aip/ipc/null_dacl.h"

namespace aip::ipc {

bool ManualEvent::open(const std::wstring& name, ManualEvent& out) {
    // Sec. 4.4 step 1: EVENT_ALL_ACCESS, bInheritHandle TRUE, exactly as the reference does.
    HANDLE h = ::OpenEventW(EVENT_ALL_ACCESS, TRUE, name.c_str());
    if (h == nullptr) {
        return false;
    }
    out.close();
    out.handle_ = h;
    return true;
}

bool ManualEvent::create(const std::wstring& name, bool initiallySignaled, ManualEvent& out) {
    NullDacl dacl;
    HANDLE h = ::CreateEventW(dacl.securityAttributes(), /*bManualReset=*/TRUE,
                              initiallySignaled ? TRUE : FALSE, name.c_str());
    if (h == nullptr) {
        return false;
    }
    out.close();
    out.handle_ = h;
    return true;
}

bool ManualEvent::createLocal(bool initiallySignaled, ManualEvent& out) {
    HANDLE h = ::CreateEventW(nullptr, /*bManualReset=*/TRUE, initiallySignaled ? TRUE : FALSE,
                              nullptr);
    if (h == nullptr) {
        return false;
    }
    out.close();
    out.handle_ = h;
    return true;
}

} // namespace aip::ipc
