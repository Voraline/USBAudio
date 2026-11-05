//! Mirrors windows/src/DefaultDeviceWatcher.cpp: an IMMNotificationClient
//! that signals an event when the default render console endpoint changes.

use windows::Win32::Foundation::{HANDLE, PROPERTYKEY};
use windows::Win32::Media::Audio::{
    eConsole, eRender, DEVICE_STATE, EDataFlow, ERole, IMMDeviceEnumerator, IMMNotificationClient,
    IMMNotificationClient_Impl,
};
use windows::Win32::System::Threading::{CreateEventW, SetEvent};
use windows::core::{implement, ComObject, PCWSTR, Result};

#[implement(IMMNotificationClient)]
pub struct DefaultDeviceWatcher {
    change_event: HANDLE,
}

impl DefaultDeviceWatcher {
    pub fn start(enumerator: &IMMDeviceEnumerator) -> Result<ComObject<Self>> {
        let change_event = unsafe { CreateEventW(None, true, false, None) }?;
        let watcher: ComObject<Self> = ComObject::new(Self { change_event });
        let client: IMMNotificationClient = watcher.to_interface();
        unsafe {
            enumerator.RegisterEndpointNotificationCallback(&client)?;
        }
        Ok(watcher)
    }

    pub fn stop(watcher: &ComObject<Self>, enumerator: &IMMDeviceEnumerator) {
        let client: IMMNotificationClient = watcher.to_interface();
        unsafe {
            let _ = enumerator.UnregisterEndpointNotificationCallback(&client);
        }
    }

    pub fn get_event(&self) -> HANDLE {
        self.change_event
    }
}

impl Drop for DefaultDeviceWatcher {
    fn drop(&mut self) {
        if self.change_event.is_invalid() {
            return;
        }
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(self.change_event);
        }
    }
}

impl IMMNotificationClient_Impl for DefaultDeviceWatcher_Impl {
    fn OnDeviceStateChanged(&self, _device_id: &PCWSTR, _new_state: DEVICE_STATE) -> Result<()> {
        Ok(())
    }

    fn OnDeviceAdded(&self, _device_id: &PCWSTR) -> Result<()> {
        Ok(())
    }

    fn OnDeviceRemoved(&self, _device_id: &PCWSTR) -> Result<()> {
        Ok(())
    }

    fn OnDefaultDeviceChanged(&self, flow: EDataFlow, role: ERole, _default_device_id: &PCWSTR) -> Result<()> {
        if flow == eRender && role == eConsole {
            unsafe {
                let _ = SetEvent(self.change_event);
            }
        }
        Ok(())
    }

    fn OnPropertyValueChanged(&self, _device_id: &PCWSTR, _key: &PROPERTYKEY) -> Result<()> {
        Ok(())
    }
}
