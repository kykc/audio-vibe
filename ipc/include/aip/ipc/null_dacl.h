// The null DACL that protocol v1's shared objects are created with (design_doc.md sec. 4.2).
//
// A null DACL grants everyone full access: any process on the machine can open these objects,
// steal the stream, inject audio, or wedge `audiodg.exe`. That is an inherited defect (sec. 3.7.2)
// which cannot be fixed without a protocol v2 (sec. 9.2) -- the deployed APO creates its objects
// this way, and we must be able to open what it created. Keep it confined to this one header
// so the eventual repair has a single site.

#pragma once

#include <windows.h>

namespace aip::ipc {

class NullDacl {
public:
    NullDacl() noexcept {
        ::InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION);
        ::SetSecurityDescriptorDacl(&descriptor_, TRUE, nullptr, FALSE);
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
    }

    NullDacl(const NullDacl&) = delete;
    NullDacl& operator=(const NullDacl&) = delete;

    [[nodiscard]] SECURITY_ATTRIBUTES* securityAttributes() noexcept { return &attributes_; }

private:
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

} // namespace aip::ipc
