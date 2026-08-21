// RAII wrapper over the 1 MiB shared file mapping of protocol v1 (design_doc.md sec. 4.2).
//
// Both sides call `CreateFileMapping(INVALID_HANDLE_VALUE, ...)` with the same name and size;
// the second caller attaches to the existing section. `createdNew()` distinguishes the two,
// which the king needs in order to decide whether to zero `valetId` (sec. 4.5).
//
// Objects are created with a null DACL to match the deployed APO (sec. 4.2). That is a known
// defect of v1, recorded in sec. 3.7.2 and slated for repair in sec. 9.2 -- it is reproduced here
// deliberately, because the valet must be able to open what the APO created.

#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <utility>

namespace aip::ipc {

class SharedMapping {
public:
    SharedMapping() = default;

    ~SharedMapping() { close(); }

    SharedMapping(SharedMapping&& other) noexcept
        : mapping_(std::exchange(other.mapping_, nullptr)),
          view_(std::exchange(other.view_, nullptr)), size_(std::exchange(other.size_, 0)),
          createdNew_(std::exchange(other.createdNew_, false)) {}

    SharedMapping& operator=(SharedMapping&& other) noexcept {
        if (this != &other) {
            close();
            mapping_ = std::exchange(other.mapping_, nullptr);
            view_ = std::exchange(other.view_, nullptr);
            size_ = std::exchange(other.size_, 0);
            createdNew_ = std::exchange(other.createdNew_, false);
        }
        return *this;
    }

    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;

    [[nodiscard]] static bool openOrCreate(const std::wstring& name, std::size_t size,
                                           SharedMapping& out);

    [[nodiscard]] bool valid() const noexcept { return view_ != nullptr; }

    /// True when this process created the section rather than attaching to an existing one.
    [[nodiscard]] bool createdNew() const noexcept { return createdNew_; }

    [[nodiscard]] void* data() const noexcept { return view_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /// Faults every page of the view into this process's working set before the first block, so
    /// the audio thread never takes a first-touch fault (sec. 7.4.1, sec. 7.4.2). Read-only
    /// touching: writing would corrupt a live stream's payload.
    void prefault() const noexcept;

    void close() noexcept;

private:
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    std::size_t size_ = 0;
    bool createdNew_ = false;
};

} // namespace aip::ipc
