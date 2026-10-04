#pragma once

#include <atomic>

#include <mmdeviceapi.h>
#include <windows.h>

class DefaultDeviceWatcher final : public IMMNotificationClient
{
public:
    DefaultDeviceWatcher() = default;
    DefaultDeviceWatcher(const DefaultDeviceWatcher&) = delete;
    DefaultDeviceWatcher& operator=(const DefaultDeviceWatcher&) = delete;

    bool Start(IMMDeviceEnumerator* Enumerator);
    void Stop();
    HANDLE GetEvent() const;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID InterfaceId, void** Object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR DeviceId, DWORD NewState) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR DeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR DeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow Flow, ERole Role, LPCWSTR DefaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR DeviceId, const PROPERTYKEY Key) override;

private:
    ~DefaultDeviceWatcher();

    std::atomic<ULONG> References{1};
    IMMDeviceEnumerator* Registered = nullptr;
    HANDLE ChangeEvent = nullptr;
};
