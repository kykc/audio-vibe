#include "aip/ipc/thread_priority.h"

namespace aip::ipc {

namespace {

// Sec. 4.6 specifies the literal value 15 rather than a symbolic constant; for a process in the
// normal priority class this is THREAD_PRIORITY_TIME_CRITICAL. Kept literal to match.
constexpr int kValetThreadPriority = 15;

} // namespace

ProAudioPriority::ProAudioPriority() noexcept {
    priorityRaised_ = ::SetThreadPriority(::GetCurrentThread(), kValetThreadPriority) != 0;

    DWORD taskIndex = 0;
    task_ = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (task_ != nullptr) {
        ::AvSetMmThreadPriority(task_, AVRT_PRIORITY_CRITICAL);
    }
}

ProAudioPriority::~ProAudioPriority() {
    if (task_ != nullptr) {
        ::AvRevertMmThreadCharacteristics(task_);
        task_ = nullptr;
    }
}

} // namespace aip::ipc
