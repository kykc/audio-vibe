#include "aip/ipc/shared_mapping.h"

#include "aip/ipc/null_dacl.h"

#include <cstdint>

namespace aip::ipc {

bool SharedMapping::openOrCreate(const std::wstring& name, std::size_t size,
                                 SharedMapping& out) {
    NullDacl dacl;

    ::SetLastError(ERROR_SUCCESS);
    HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, dacl.securityAttributes(),
                                          PAGE_READWRITE, 0, static_cast<DWORD>(size),
                                          name.c_str());
    if (mapping == nullptr) {
        return false;
    }
    const bool createdNew = ::GetLastError() != ERROR_ALREADY_EXISTS;

    void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (view == nullptr) {
        ::CloseHandle(mapping);
        return false;
    }

    out.close();
    out.mapping_ = mapping;
    out.view_ = view;
    out.size_ = size;
    out.createdNew_ = createdNew;
    return true;
}

void SharedMapping::prefault() const noexcept {
    if (view_ == nullptr) {
        return;
    }

    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    const std::size_t pageSize = info.dwPageSize != 0 ? info.dwPageSize : 4096u;

    const auto* bytes = static_cast<const volatile unsigned char*>(view_);
    volatile unsigned char sink = 0;
    for (std::size_t offset = 0; offset < size_; offset += pageSize) {
        sink = static_cast<unsigned char>(sink ^ bytes[offset]);
    }
    (void)sink;
}

void SharedMapping::close() noexcept {
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    size_ = 0;
    createdNew_ = false;
}

} // namespace aip::ipc
