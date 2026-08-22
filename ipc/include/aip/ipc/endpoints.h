// Render endpoint discovery. The protocol v1 object names are derived from the endpoint GUID as
// returned by `PKEY_AudioEndpoint_GUID`, used verbatim including braces (design_doc.md sec. 4.2),
// which is exactly the value the APO reads on its side. This is the client's only use of the
// MMDevice API and it is strictly control-thread work: COM activation is forbidden on the audio
// thread (sec. 7.4.1).

#pragma once

#include <cstdint>
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

    /// The endpoint's configured channel mask -- `dwChannelMask` of the `WAVEFORMATEXTENSIBLE`
    /// in `PKEY_AudioEngine_DeviceFormat`, i.e. the `SPEAKER_*` bits that say which speaker each
    /// channel of the stream drives. Zero means "unknown": the device reports a plain
    /// `WAVEFORMATEX`, or the property is missing, both of which are legal.
    ///
    /// This is the only channel-*order* information available anywhere in the system, because
    /// protocol v1 carries none (sec. 4.3) and its header is frozen. It exists here rather than
    /// on the wire because the object names are derived from the endpoint GUID (sec. 4.2), so a
    /// valet that can attach at all already knows exactly which device to ask.
    std::uint32_t channelMask = 0;

    /// `nChannels` from the same format blob. Kept beside the mask so a caller can reject a
    /// stale mask rather than trust it: the device format is what the endpoint is *configured*
    /// for, which is not a promise about the block in front of the audio thread.
    std::uint32_t deviceChannelCount = 0;
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
