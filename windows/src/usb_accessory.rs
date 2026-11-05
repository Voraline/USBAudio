//! Mirrors windows/src/UsbAccessory.cpp: USB device discovery, Android Open
//! Accessory negotiation, and bulk-pipe audio transport over WinUSB.

use std::mem::size_of;
use std::ptr::null_mut;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::sleep;
use std::time::{Duration, Instant};

use windows::Win32::Devices::DeviceAndDriverInstallation::{
    SetupDiDestroyDeviceInfoList, SetupDiEnumDeviceInfo, SetupDiEnumDeviceInterfaces,
    SetupDiGetClassDevsW, SetupDiGetDeviceInstanceIdW, SetupDiGetDeviceInterfaceDetailW,
    SetupDiOpenDevRegKey, DICS_FLAG_GLOBAL, DIGCF_ALLCLASSES, DIGCF_DEVICEINTERFACE,
    DIGCF_PRESENT, DIREG_DEV, SP_DEVICE_INTERFACE_DATA,
    SP_DEVICE_INTERFACE_DETAIL_DATA_W, SP_DEVINFO_DATA,
};
use windows::Win32::Devices::Usb::{
    UsbdPipeTypeBulk, WinUsb_AbortPipe, WinUsb_ControlTransfer, WinUsb_Free,
    WinUsb_GetOverlappedResult, WinUsb_Initialize, WinUsb_QueryInterfaceSettings, WinUsb_QueryPipe,
    WinUsb_ReadPipe, WinUsb_SetPipePolicy, WinUsb_SetPowerPolicy, WinUsb_WritePipe, AUTO_SUSPEND,
    PIPE_TRANSFER_TIMEOUT, SHORT_PACKET_TERMINATE, USB_INTERFACE_DESCRIPTOR,
    WINUSB_INTERFACE_HANDLE, WINUSB_PIPE_INFORMATION, WINUSB_SETUP_PACKET,
};
use windows::Win32::Foundation::{
    CloseHandle, GetLastError, GENERIC_READ, GENERIC_WRITE, ERROR_IO_PENDING,
    ERROR_OPERATION_ABORTED, ERROR_SEM_TIMEOUT, ERROR_SUCCESS, ERROR_TIMEOUT, HANDLE,
    INVALID_HANDLE_VALUE, WAIT_OBJECT_0,
};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_FLAG_OVERLAPPED, FILE_SHARE_READ, FILE_SHARE_WRITE,
    OPEN_EXISTING,
};
use windows::Win32::System::IO::OVERLAPPED;
use windows::Win32::System::Registry::{
    RegCloseKey, RegQueryValueExW, KEY_QUERY_VALUE, REG_MULTI_SZ, REG_SZ,
};
use windows::Win32::System::Threading::{CreateEventW, WaitForSingleObject};
use windows::core::{GUID, PCWSTR};

use usb_audio_shared::{MAX_PACKET_SAMPLES, RECEIVER_READY};

use crate::util::{SendHandle, SendOverlapped, SendPtr};

const GOOGLE_VENDOR_ID: u16 = 0x18D1;
const ACCESSORY_PRODUCT_ID: u16 = 0x2D00;
const ACCESSORY_ADB_PRODUCT_ID: u16 = 0x2D01;

const WRITE_SLOT_COUNT: usize = 4;
const PIPE_TIMEOUT_MS: u32 = 350;

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct UsbDeviceId {
    pub vendor_id: u16,
    pub product_id: u16,
}

struct WriteSlot {
    overlapped: SendOverlapped,
    event: SendHandle,
    size: u32,
    pending: bool,
    buffer: [u8; MAX_PACKET_SAMPLES * size_of::<i16>()],
}

impl Default for WriteSlot {
    fn default() -> Self {
        Self {
            overlapped: SendOverlapped(OVERLAPPED::default()),
            event: SendHandle(HANDLE(null_mut())),
            size: 0,
            pending: false,
            buffer: [0u8; MAX_PACKET_SAMPLES * size_of::<i16>()],
        }
    }
}

fn make_id(vendor_id: u16, product_id: u16) -> String {
    format!("VID_{:04X}&PID_{:04X}", vendor_id, product_id)
}

fn uppercase(mut value: String) -> String {
    value.make_ascii_uppercase();
    value
}

fn wide_to_string(data: &[u16]) -> String {
    let end = data.iter().position(|&c| c == 0).unwrap_or(data.len());
    String::from_utf16_lossy(&data[..end])
}

fn to_pcwstr(data: &[u16]) -> Vec<u16> {
    let mut buffer: Vec<u16> = data.to_vec();
    if buffer.last() != Some(&0) {
        buffer.push(0);
    }
    buffer
}

fn contains_id(device_path: &str, vendor_id: u16, product_id: u16) -> bool {
    uppercase(device_path.to_string()).contains(&make_id(vendor_id, product_id))
}

fn is_timeout_error(error: windows::Win32::Foundation::WIN32_ERROR) -> bool {
    error == ERROR_SEM_TIMEOUT || error == ERROR_TIMEOUT || error == ERROR_OPERATION_ABORTED
}

fn parse_guid(text: &str) -> Option<GUID> {
    let text = text.trim();
    let text = text.strip_prefix('{').unwrap_or(text);
    let text = text.strip_suffix('}').unwrap_or(text);
    let parts: Vec<&str> = text.split('-').collect();
    if parts.len() != 5 {
        return None;
    }
    let data1 = u32::from_str_radix(parts[0], 16).ok()?;
    let data2 = u16::from_str_radix(parts[1], 16).ok()?;
    let data3 = u16::from_str_radix(parts[2], 16).ok()?;
    let group4 = u16::from_str_radix(parts[3], 16).ok()?;
    let group5 = u64::from_str_radix(parts[4], 16).ok()?;
    Some(GUID {
        data1,
        data2,
        data3,
        data4: [
            (group4 >> 8) as u8,
            group4 as u8,
            (group5 >> 40) as u8,
            (group5 >> 32) as u8,
            (group5 >> 24) as u8,
            (group5 >> 16) as u8,
            (group5 >> 8) as u8,
            group5 as u8,
        ],
    })
}

fn append_interface_guids(
    registry_key: windows::Win32::System::Registry::HKEY,
    value_name: PCWSTR,
    interface_guids: &mut Vec<GUID>,
) {
    let mut data_type = windows::Win32::System::Registry::REG_VALUE_TYPE(0);
    let mut data_size = 0u32;
    let result = unsafe {
        RegQueryValueExW(
            registry_key,
            value_name,
            None,
            Some(&mut data_type),
            None,
            Some(&mut data_size),
        )
    };
    if result != ERROR_SUCCESS
        || (data_type != REG_SZ && data_type != REG_MULTI_SZ)
        || data_size < size_of::<u16>() as u32
    {
        return;
    }
    let mut data = vec![0u16; data_size as usize / size_of::<u16>() + 1];
    let result = unsafe {
        RegQueryValueExW(
            registry_key,
            value_name,
            None,
            Some(&mut data_type),
            Some(data.as_mut_ptr() as *mut u8),
            Some(&mut data_size),
        )
    };
    if result != ERROR_SUCCESS {
        return;
    }
    let character_count = data_size as usize / size_of::<u16>();
    let mut offset = 0usize;
    while offset < character_count {
        if data[offset] == 0 {
            offset += 1;
            continue;
        }
        let mut end = offset;
        while end < character_count && data[end] != 0 {
            end += 1;
        }
        let value = String::from_utf16_lossy(&data[offset..end]);
        if let Some(interface_guid) = parse_guid(&value) {
            if !interface_guids.iter().any(|existing| existing == &interface_guid) {
                interface_guids.push(interface_guid);
            }
        }
        offset = end + 1;
    }
}

fn find_interface_guids(vendor_id: u16, product_id: u16) -> Vec<GUID> {
    let mut interface_guids = Vec::new();
    let device_info = match unsafe {
        SetupDiGetClassDevsW(
            None,
            windows::core::w!("USB"),
            None,
            DIGCF_ALLCLASSES | DIGCF_PRESENT,
        )
    } {
        Ok(info) => info,
        Err(_) => return interface_guids,
    };
    if device_info.0 == -1 {
        return interface_guids;
    }
    for index in 0u32.. {
        let mut device_data = SP_DEVINFO_DATA {
            cbSize: size_of::<SP_DEVINFO_DATA>() as u32,
            ..Default::default()
        };
        if unsafe { SetupDiEnumDeviceInfo(device_info, index, &mut device_data) }.is_err() {
            break;
        }
        let mut required_size = 0u32;
        let _ = unsafe {
            SetupDiGetDeviceInstanceIdW(device_info, &device_data, None, Some(&mut required_size))
        };
        if required_size == 0 {
            continue;
        }
        let mut instance_id = vec![0u16; required_size as usize + 1];
        if unsafe {
            SetupDiGetDeviceInstanceIdW(
                device_info,
                &device_data,
                Some(&mut instance_id[..]),
                None,
            )
        }
        .is_err()
            || !contains_id(&wide_to_string(&instance_id), vendor_id, product_id)
        {
            continue;
        }
        let registry_key = unsafe {
            SetupDiOpenDevRegKey(
                device_info,
                &device_data,
                DICS_FLAG_GLOBAL.0,
                0,
                DIREG_DEV,
                KEY_QUERY_VALUE.0,
            )
        };
        match registry_key {
            Ok(key) => {
                append_interface_guids(key, windows::core::w!("DeviceInterfaceGUIDs"), &mut interface_guids);
                append_interface_guids(key, windows::core::w!("DeviceInterfaceGUID"), &mut interface_guids);
                unsafe {
                    let _ = RegCloseKey(key);
                }
            }
            Err(_) => continue,
        }
    }
    unsafe {
        let _ = SetupDiDestroyDeviceInfoList(device_info);
    }
    interface_guids
}

/// Parses an instance ID of the form `USB\VID_xxxx&PID_xxxx`, matching the
/// `swscanf_s(L"USB\\VID_%4x&PID_%4x")` pattern used by the C++ version.
fn parse_usb_instance_id(instance_id: &str) -> Option<(u16, u16)> {
    let upper = uppercase(instance_id.to_string());
    let rest = upper.strip_prefix("USB\\VID_")?;
    let mut chars = rest.chars();
    let mut vendor = 0u32;
    let mut digits = 0;
    for ch in chars.by_ref() {
        let Some(value) = ch.to_digit(16) else { break };
        vendor = vendor * 16 + value;
        digits += 1;
        if digits == 4 {
            break;
        }
    }
    if digits == 0 {
        return None;
    }
    let remaining: String = chars.collect();
    let rest = remaining.strip_prefix("&PID_")?;
    let mut product = 0u32;
    let mut digits = 0;
    for ch in rest.chars() {
        let Some(value) = ch.to_digit(16) else { break };
        product = product * 16 + value;
        digits += 1;
        if digits == 4 {
            break;
        }
    }
    if digits == 0 {
        return None;
    }
    Some((vendor as u16, product as u16))
}

pub fn find_usb_device_ids() -> Vec<UsbDeviceId> {
    let mut device_ids = Vec::new();
    let device_info = match unsafe {
        SetupDiGetClassDevsW(
            None,
            windows::core::w!("USB"),
            None,
            DIGCF_ALLCLASSES | DIGCF_PRESENT,
        )
    } {
        Ok(info) => info,
        Err(_) => return device_ids,
    };
    if device_info.0 == -1 {
        return device_ids;
    }
    for index in 0u32.. {
        let mut device_data = SP_DEVINFO_DATA {
            cbSize: size_of::<SP_DEVINFO_DATA>() as u32,
            ..Default::default()
        };
        if unsafe { SetupDiEnumDeviceInfo(device_info, index, &mut device_data) }.is_err() {
            break;
        }
        let mut instance_id = [0u16; 256];
        if unsafe {
            SetupDiGetDeviceInstanceIdW(
                device_info,
                &device_data,
                Some(&mut instance_id[..]),
                None,
            )
        }
        .is_err()
        {
            continue;
        }
        let Some((vendor_id, product_id)) =
            parse_usb_instance_id(&wide_to_string(&instance_id))
        else {
            continue;
        };
        let device_id = UsbDeviceId {
            vendor_id,
            product_id,
        };
        if device_id.vendor_id == GOOGLE_VENDOR_ID
            && (device_id.product_id == ACCESSORY_PRODUCT_ID
                || device_id.product_id == ACCESSORY_ADB_PRODUCT_ID)
        {
            continue;
        }
        if device_ids.contains(&device_id) {
            continue;
        }
        if find_interface_guids(device_id.vendor_id, device_id.product_id).is_empty() {
            continue;
        }
        device_ids.push(device_id);
    }
    unsafe {
        let _ = SetupDiDestroyDeviceInfoList(device_info);
    }
    device_ids
}

pub struct UsbAccessoryLink {
    device_handle: SendHandle,
    interface_handle: SendPtr<core::ffi::c_void>,
    out_pipe: u8,
    in_pipe: u8,
    write_slots: [WriteSlot; WRITE_SLOT_COUNT],
    next_write_slot: usize,
}

impl Default for UsbAccessoryLink {
    fn default() -> Self {
        Self {
            device_handle: SendHandle(INVALID_HANDLE_VALUE),
            interface_handle: SendPtr(null_mut()),
            out_pipe: 0,
            in_pipe: 0,
            write_slots: std::array::from_fn(|_| WriteSlot::default()),
            next_write_slot: 0,
        }
    }
}

impl UsbAccessoryLink {
    pub fn enter_accessory_mode(&mut self, vendor_id: u16, product_id: u16) -> bool {
        if !self.open_matching_device(vendor_id, product_id, None) {
            return false;
        }
        let interface_handle = self.interface_handle();
        let mut setup = WINUSB_SETUP_PACKET {
            RequestType: 0xC0,
            Request: 51,
            Length: size_of::<u16>() as u16,
            ..Default::default()
        };
        let mut buffer = [0u8; size_of::<u16>()];
        let mut transferred = 0u32;
        if unsafe {
            WinUsb_ControlTransfer(interface_handle, setup, Some(&mut buffer), Some(&mut transferred), None)
        }
        .is_err()
            || transferred < size_of::<u16>() as u32
        {
            self.close();
            return false;
        }
        let accessory_protocol_version = u16::from_le_bytes([buffer[0], buffer[1]]);
        if accessory_protocol_version < 1 {
            self.close();
            return false;
        }
        const STRINGS: [&str; 6] = [
            "OpenAI",
            "USB Audio Stream",
            "USB PCM Audio",
            "",
            "https://github.com/",
            "USBAudio",
        ];
        setup = WINUSB_SETUP_PACKET {
            RequestType: 0x40,
            Request: 52,
            ..Default::default()
        };
        for (index, value) in STRINGS.iter().enumerate() {
            setup.Value = 0;
            setup.Index = index as u16;
            let mut utf8 = value.as_bytes().to_vec();
            utf8.push(0);
            setup.Length = utf8.len() as u16;
            if unsafe {
                WinUsb_ControlTransfer(
                    interface_handle,
                    setup,
                    Some(&mut utf8),
                    Some(&mut transferred),
                    None,
                )
            }
            .is_err()
            {
                self.close();
                return false;
            }
        }
        setup = WINUSB_SETUP_PACKET {
            RequestType: 0x40,
            Request: 53,
            ..Default::default()
        };
        if unsafe { WinUsb_ControlTransfer(interface_handle, setup, None, Some(&mut transferred), None) }.is_err() {
            self.close();
            return false;
        }
        self.close();
        true
    }

    pub fn try_open_accessory(&mut self) -> bool {
        for product_id in [ACCESSORY_PRODUCT_ID, ACCESSORY_ADB_PRODUCT_ID] {
            if self.open_matching_device(GOOGLE_VENDOR_ID, product_id, Some(0)) {
                if self.select_bulk_pipes() {
                    return true;
                }
                self.close();
            }
        }
        false
    }

    pub fn wait_for_accessory(&mut self, running: &AtomicBool, timeout_milliseconds: u32) -> bool {
        let deadline = Instant::now() + Duration::from_millis(timeout_milliseconds as u64);
        while running.load(Ordering::Acquire) && Instant::now() < deadline {
            if self.try_open_accessory() {
                return true;
            }
            sleep(Duration::from_millis(100));
        }
        false
    }

    pub fn wait_for_receiver(&mut self, running: &AtomicBool, timeout_milliseconds: u32) -> bool {
        let interface_handle = self.interface_handle();
        if interface_handle.0.is_null() || self.in_pipe == 0 {
            return false;
        }
        let deadline = Instant::now() + Duration::from_millis(timeout_milliseconds as u64);
        while running.load(Ordering::Acquire)
            && (timeout_milliseconds == 0 || Instant::now() < deadline)
        {
            let mut buffer = [0u8; size_of::<u8>()];
            let mut transferred = 0u32;
            let result = unsafe {
                WinUsb_ReadPipe(
                    interface_handle,
                    self.in_pipe,
                    Some(&mut buffer),
                    Some(&mut transferred),
                    None,
                )
            };
            if result.is_ok() {
                if transferred == size_of::<u8>() as u32 && buffer[0] == RECEIVER_READY {
                    return true;
                }
                continue;
            }
            let error = unsafe { GetLastError() };
            if !is_timeout_error(error) {
                return false;
            }
        }
        false
    }

    pub fn write_audio(&mut self, samples: &[i16]) -> bool {
        if self.interface_handle.0.is_null() {
            return false;
        }
        if samples.is_empty() {
            return true;
        }
        // SAFETY: i16 slices are always valid byte slices.
        let bytes =
            unsafe { std::slice::from_raw_parts(samples.as_ptr() as *const u8, samples.len() * 2) };
        self.write_bytes(bytes)
    }

    pub fn cancel_transfers(&mut self) {
        let interface_handle = self.interface_handle();
        if !interface_handle.0.is_null() {
            if self.out_pipe != 0 {
                unsafe {
                    let _ = WinUsb_AbortPipe(interface_handle, self.out_pipe);
                }
            }
            if self.in_pipe != 0 {
                unsafe {
                    let _ = WinUsb_AbortPipe(interface_handle, self.in_pipe);
                }
            }
        }
    }

    pub fn close(&mut self) {
        let interface_handle = self.interface_handle();
        if !interface_handle.0.is_null() {
            self.cancel_transfers();
            for slot in &mut self.write_slots {
                if slot.pending {
                    let mut transferred = 0u32;
                    unsafe {
                        let _ = WinUsb_GetOverlappedResult(
                            interface_handle,
                            &slot.overlapped.0,
                            &mut transferred,
                            true,
                        );
                    }
                    slot.pending = false;
                }
            }
            unsafe {
                let _ = WinUsb_Free(interface_handle);
            }
            self.interface_handle = SendPtr(null_mut());
        }
        for slot in &mut self.write_slots {
            slot.pending = false;
            if slot.event.0.is_invalid() {
                continue;
            }
            unsafe {
                let _ = CloseHandle(slot.event.0);
            }
            slot.event = SendHandle(HANDLE(null_mut()));
        }
        self.next_write_slot = 0;
        if self.device_handle.0 != INVALID_HANDLE_VALUE {
            unsafe {
                let _ = CloseHandle(self.device_handle.0);
            }
            self.device_handle = SendHandle(INVALID_HANDLE_VALUE);
        }
        self.out_pipe = 0;
        self.in_pipe = 0;
    }

    fn interface_handle(&self) -> WINUSB_INTERFACE_HANDLE {
        WINUSB_INTERFACE_HANDLE(self.interface_handle.0)
    }

    fn open_matching_device(
        &mut self,
        vendor_id: u16,
        product_id: u16,
        required_interface_number: Option<u8>,
    ) -> bool {
        let interface_guids = find_interface_guids(vendor_id, product_id);
        for interface_guid in &interface_guids {
            let device_info = match unsafe {
                SetupDiGetClassDevsW(
                    Some(interface_guid),
                    PCWSTR::null(),
                    None,
                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
                )
            } {
                Ok(info) => info,
                Err(_) => continue,
            };
            if device_info.0 == -1 {
                continue;
            }
            let mut opened = false;
            for index in 0u32.. {
                let mut interface_data = SP_DEVICE_INTERFACE_DATA {
                    cbSize: size_of::<SP_DEVICE_INTERFACE_DATA>() as u32,
                    ..Default::default()
                };
                if unsafe {
                    SetupDiEnumDeviceInterfaces(
                        device_info,
                        None,
                        interface_guid,
                        index,
                        &mut interface_data,
                    )
                }
                .is_err()
                {
                    break;
                }
                let mut required_size = 0u32;
                let _ = unsafe {
                    SetupDiGetDeviceInterfaceDetailW(
                        device_info,
                        &interface_data,
                        None,
                        0,
                        Some(&mut required_size),
                        None,
                    )
                };
                if required_size == 0 {
                    continue;
                }
                let mut detail_buffer = vec![0u8; required_size as usize];
                let detail = detail_buffer.as_mut_ptr() as *mut SP_DEVICE_INTERFACE_DETAIL_DATA_W;
                unsafe {
                    (*detail).cbSize = size_of::<SP_DEVICE_INTERFACE_DETAIL_DATA_W>() as u32;
                }
                if unsafe {
                    SetupDiGetDeviceInterfaceDetailW(
                        device_info,
                        &interface_data,
                        Some(detail),
                        required_size,
                        None,
                        None,
                    )
                }
                .is_err()
                {
                    continue;
                }
                let path = unsafe {
                    let ptr = (*detail).DevicePath.as_ptr();
                    let mut len = 0;
                    while len < required_size as usize / size_of::<u16>() && *ptr.add(len) != 0 {
                        len += 1;
                    }
                    std::slice::from_raw_parts(ptr, len)
                };
                if !contains_id(&wide_to_string(path), vendor_id, product_id) {
                    continue;
                }
                if !self.open_path(path) {
                    continue;
                }
                if let Some(required) = required_interface_number {
                    let interface_handle = self.interface_handle();
                    let mut descriptor = USB_INTERFACE_DESCRIPTOR::default();
                    if unsafe {
                        WinUsb_QueryInterfaceSettings(interface_handle, 0, &mut descriptor)
                    }
                    .is_err()
                        || descriptor.bInterfaceNumber != required
                    {
                        self.close();
                        continue;
                    }
                }
                opened = true;
                break;
            }
            unsafe {
                let _ = SetupDiDestroyDeviceInfoList(device_info);
            }
            if opened {
                return true;
            }
        }
        false
    }

    fn open_path(&mut self, device_path: &[u16]) -> bool {
        self.close();
        let path = to_pcwstr(device_path);
        let handle = unsafe {
            CreateFileW(
                PCWSTR(path.as_ptr()),
                GENERIC_READ.0 | GENERIC_WRITE.0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                None,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                None,
            )
        };
        let handle = match handle {
            Ok(handle) => handle,
            Err(_) => return false,
        };
        self.device_handle = SendHandle(handle);
        let mut interface_handle: WINUSB_INTERFACE_HANDLE = WINUSB_INTERFACE_HANDLE(null_mut());
        if unsafe { WinUsb_Initialize(handle, &mut interface_handle) }.is_err() {
            self.close();
            return false;
        }
        self.interface_handle = SendPtr(interface_handle.0);
        true
    }

    fn select_bulk_pipes(&mut self) -> bool {
        let interface_handle = self.interface_handle();
        let mut descriptor = USB_INTERFACE_DESCRIPTOR::default();
        if unsafe { WinUsb_QueryInterfaceSettings(interface_handle, 0, &mut descriptor) }.is_err()
            || descriptor.bInterfaceNumber != 0
        {
            return false;
        }
        self.out_pipe = 0;
        self.in_pipe = 0;
        for index in 0..descriptor.bNumEndpoints {
            let mut pipe = WINUSB_PIPE_INFORMATION::default();
            if unsafe { WinUsb_QueryPipe(interface_handle, 0, index, &mut pipe) }.is_err()
                || pipe.PipeType != UsbdPipeTypeBulk
            {
                continue;
            }
            if pipe.PipeId & 0x80 == 0 {
                self.out_pipe = pipe.PipeId;
            } else {
                self.in_pipe = pipe.PipeId;
            }
        }
        if self.out_pipe == 0 || self.in_pipe == 0 {
            return false;
        }
        let timeout_ms: u32 = PIPE_TIMEOUT_MS;
        unsafe {
            let _ = WinUsb_SetPipePolicy(
                interface_handle,
                self.in_pipe,
                PIPE_TRANSFER_TIMEOUT,
                size_of::<u32>() as u32,
                (&timeout_ms as *const u32) as *const core::ffi::c_void,
            );
        }
        let short_packet_terminate: u8 = 1;
        unsafe {
            let _ = WinUsb_SetPipePolicy(
                interface_handle,
                self.out_pipe,
                SHORT_PACKET_TERMINATE,
                size_of::<u8>() as u32,
                (&short_packet_terminate as *const u8) as *const core::ffi::c_void,
            );
        }
        let auto_suspend: u8 = 0;
        unsafe {
            let _ = WinUsb_SetPowerPolicy(
                interface_handle,
                AUTO_SUSPEND,
                size_of::<u8>() as u32,
                (&auto_suspend as *const u8) as *const core::ffi::c_void,
            );
        }
        self.create_write_slots()
    }

    fn create_write_slots(&mut self) -> bool {
        for slot in &mut self.write_slots {
            if slot.event.0.is_invalid() {
                let event = unsafe { CreateEventW(None, true, false, None) };
                match event {
                    Ok(event) => slot.event = SendHandle(event),
                    Err(_) => return false,
                }
            }
            slot.pending = false;
        }
        self.next_write_slot = 0;
        true
    }

    fn complete_write(
        interface_handle: WINUSB_INTERFACE_HANDLE,
        out_pipe: u8,
        slot: &mut WriteSlot,
        timeout_milliseconds: u32,
    ) -> bool {
        if !slot.pending {
            return true;
        }
        let wait_result = unsafe { WaitForSingleObject(slot.event.0, timeout_milliseconds) };
        if wait_result != WAIT_OBJECT_0 {
            unsafe {
                let _ = WinUsb_AbortPipe(interface_handle, out_pipe);
            }
            let mut discarded = 0u32;
            unsafe {
                let _ = WinUsb_GetOverlappedResult(
                    interface_handle,
                    &slot.overlapped.0,
                    &mut discarded,
                    true,
                );
            }
            slot.pending = false;
            return false;
        }
        let mut transferred = 0u32;
        let completed = unsafe {
            WinUsb_GetOverlappedResult(
                interface_handle,
                &slot.overlapped.0,
                &mut transferred,
                false,
            )
        }
        .is_ok();
        slot.pending = false;
        completed && transferred == slot.size
    }

    fn write_bytes(&mut self, data: &[u8]) -> bool {
        let size = data.len();
        if size > self.write_slots[0].buffer.len() {
            return false;
        }
        let interface_handle = self.interface_handle();
        let out_pipe = self.out_pipe;
        let next_write_slot = self.next_write_slot;
        let slot = &mut self.write_slots[next_write_slot];
        if !Self::complete_write(interface_handle, out_pipe, slot, PIPE_TIMEOUT_MS) {
            return false;
        }
        slot.buffer[..size].copy_from_slice(data);
        slot.overlapped = SendOverlapped(OVERLAPPED::default());
        slot.overlapped.0.hEvent = slot.event.0;
        slot.size = size as u32;
        let mut transferred = 0u32;
        let result = unsafe {
            WinUsb_WritePipe(
                interface_handle,
                out_pipe,
                &slot.buffer[..size],
                Some(&mut transferred),
                Some(&slot.overlapped.0),
            )
        };
        if result.is_err() {
            if unsafe { GetLastError() } != ERROR_IO_PENDING {
                return false;
            }
            slot.pending = true;
        }
        self.next_write_slot = (next_write_slot + 1) % self.write_slots.len();
        true
    }
}

impl Drop for UsbAccessoryLink {
    fn drop(&mut self) {
        self.close();
    }
}
