#include "DefaultDeviceWatcher.h"

DefaultDeviceWatcher::~DefaultDeviceWatcher()
{
    Stop();
    if (ChangeEvent != nullptr)
    {
        CloseHandle(ChangeEvent);
        ChangeEvent = nullptr;
    }
}

bool DefaultDeviceWatcher::Start(IMMDeviceEnumerator* Enumerator)
{
    ChangeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ChangeEvent == nullptr || FAILED(Enumerator->RegisterEndpointNotificationCallback(this)))
    {
        return false;
    }
    Registered = Enumerator;
    return true;
}

void DefaultDeviceWatcher::Stop()
{
    if (Registered != nullptr)
    {
        Registered->UnregisterEndpointNotificationCallback(this);
        Registered = nullptr;
    }
}

HANDLE DefaultDeviceWatcher::GetEvent() const
{
    return ChangeEvent;
}

HRESULT STDMETHODCALLTYPE DefaultDeviceWatcher::QueryInterface(REFIID InterfaceId, void** Object)
{
    if (Object == nullptr)
    {
        return E_POINTER;
    }
    if (IsEqualIID(InterfaceId, __uuidof(IUnknown)) || IsEqualIID(InterfaceId, __uuidof(IMMNotificationClient)))
    {
        *Object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *Object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DefaultDeviceWatcher::AddRef()
{
    return ++References;
}

ULONG STDMETHODCALLTYPE DefaultDeviceWatcher::Release()
{
    const ULONG Remaining = --References;
    if (Remaining == 0)
    {
        delete this;
    }
    return Remaining;
}

HRESULT STDMETHODCALLTYPE DefaultDeviceWatcher::OnDeviceStateChanged(LPCWSTR, DWORD)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DefaultDeviceWatcher::OnDeviceAdded(LPCWSTR)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DefaultDeviceWatcher::OnDeviceRemoved(LPCWSTR)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DefaultDeviceWatcher::OnDefaultDeviceChanged(EDataFlow Flow, ERole Role, LPCWSTR)
{
    if (Flow == eRender && Role == eConsole && ChangeEvent != nullptr)
    {
        SetEvent(ChangeEvent);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DefaultDeviceWatcher::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY)
{
    return S_OK;
}
