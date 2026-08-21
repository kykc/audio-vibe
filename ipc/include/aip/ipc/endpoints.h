// Render endpoint discovery. The protocol v1 object names are derived from the endpoint GUID as
// returned by `PKEY_AudioEndpoint_GUID`, used verbatim including braces (design_doc.md sec. 4.2),
// which is exactly the value the APO reads on its side. This is the client's only use of the
// MMDevice API and it is strictly control-thread work: COM activation is forbidden on the audio
// thread (sec. 7.4.1).

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aip::ipc {

struct RenderEndpoint {
    /// `PKEY_AudioEndpoint_GUID`, verbatim and brace-wrapped. Feed to
    /// `protocol::objectBaseName` -- do not case-fold it: only the APO's *registry* lookups use
    /// a lowercased copy, the object names use this form (sec. 4.2).
    std::wstring guid;
    std::wstring friendlyName;
    bool isDefault = false;
};

/// Active render endpoints. Requires an initialised COM apartment on the calling thread.
[[nodiscard]] std::vector<RenderEndpoint> enumerateRenderEndpoints();

/// The default console-role render endpoint, if there is one.
[[nodiscard]] std::optional<RenderEndpoint> defaultRenderEndpoint();

/// RAII COM apartment for the control thread.
class ComApartment {
public:
    ComApartment() noexcept;
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
    bool needsUninitialize_ = false;
};

} // namespace aip::ipc
