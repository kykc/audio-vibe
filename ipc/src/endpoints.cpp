#include "aip/ipc/endpoints.h"

// The include order below is load-bearing and must not be alphabetised.
//
//  1. <windows.h> first, as always.
//  2. <initguid.h> next. It defines INITGUID, which flips DEFINE_GUID/DEFINE_PROPERTYKEY in
//     every header that follows from an external *declaration* into a *definition*. The
//     property keys we need have no import library, so exactly one translation unit in the
//     program must do this -- and this is it.
//  3. <mmdeviceapi.h> before <functiondiscoverykeys_devpkey.h>: the latter uses
//     DEFINE_PROPERTYKEY without including <propkeydef.h> itself (the include is commented out
//     in the SDK header), and relies on the includer having pulled it in already. mmdeviceapi.h
//     does so via <propsys.h>. Reversed, the build fails inside the SDK header with a wall of
//     C2065/C4430 that says nothing about include order.
#include <windows.h>

#include <initguid.h>

#include <mmdeviceapi.h>

#include <functiondiscoverykeys_devpkey.h>

#include <propidl.h>
#include <wrl/client.h>

namespace aip::ipc {

namespace {

using Microsoft::WRL::ComPtr;

std::wstring readStringProperty(IPropertyStore& store, const PROPERTYKEY& key) {
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(store.GetValue(key, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr) {
        result.assign(value.pwszVal);
    }
    ::PropVariantClear(&value);
    return result;
}

bool describe(IMMDevice& device, RenderEndpoint& out) {
    ComPtr<IPropertyStore> store;
    if (FAILED(device.OpenPropertyStore(STGM_READ, &store))) {
        return false;
    }
    out.guid = readStringProperty(*store.Get(), PKEY_AudioEndpoint_GUID);
    out.friendlyName = readStringProperty(*store.Get(), PKEY_Device_FriendlyName);
    return !out.guid.empty();
}

std::wstring defaultEndpointGuid(IMMDeviceEnumerator& enumerator) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator.GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return {};
    }
    RenderEndpoint info;
    return describe(*device.Get(), info) ? info.guid : std::wstring{};
}

} // namespace

ComApartment::ComApartment() noexcept {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ok_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    needsUninitialize_ = SUCCEEDED(hr);
}

ComApartment::~ComApartment() {
    if (needsUninitialize_) {
        ::CoUninitialize();
    }
}

std::vector<RenderEndpoint> enumerateRenderEndpoints() {
    std::vector<RenderEndpoint> endpoints;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator)))) {
        return endpoints;
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return endpoints;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return endpoints;
    }

    const std::wstring defaultGuid = defaultEndpointGuid(*enumerator.Get());

    endpoints.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) {
            continue;
        }
        RenderEndpoint info;
        if (!describe(*device.Get(), info)) {
            continue;
        }
        info.isDefault = !defaultGuid.empty() && info.guid == defaultGuid;
        endpoints.push_back(std::move(info));
    }

    return endpoints;
}

std::optional<RenderEndpoint> defaultRenderEndpoint() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator)))) {
        return std::nullopt;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return std::nullopt;
    }

    RenderEndpoint info;
    if (!describe(*device.Get(), info)) {
        return std::nullopt;
    }
    info.isDefault = true;
    return info;
}

} // namespace aip::ipc
