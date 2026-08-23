#include "process_footprint.h"

#include <windows.h>

// After <windows.h>, which it depends on and does not include.
#include <psapi.h>

namespace aip::ui {

ProcessFootprint processFootprint() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return {};
    }
    return {counters.WorkingSetSize, counters.PeakWorkingSetSize};
}

} // namespace aip::ui
