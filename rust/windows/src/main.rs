//! Mirrors windows/src/Main.cpp: console handling, process scheduling,
//! USB accessory auto-connect, sender thread, and capture session loop.

mod audio_capture;
mod audio_packet;
mod device_watcher;
mod low_latency_render;
mod tpdf_dither;
mod usb_accessory;
mod util;

use std::ffi::{OsStr, OsString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::System::Console::{
    SetConsoleCtrlHandler, CTRL_BREAK_EVENT, CTRL_CLOSE_EVENT, CTRL_C_EVENT, CTRL_SHUTDOWN_EVENT,
};
use windows::Win32::System::Threading::{
    AvRevertMmThreadCharacteristics, AvSetMmThreadCharacteristicsW, AvSetMmThreadPriority,
    CreateEventW, GetCurrentProcess, GetCurrentThread, SetEvent, SetPriorityClass,
    SetProcessInformation, SetThreadPriority, WaitForSingleObject, AVRT_PRIORITY_HIGH,
    HIGH_PRIORITY_CLASS, INFINITE, PROCESS_POWER_THROTTLING_CURRENT_VERSION,
    PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_STATE,
    THREAD_PRIORITY_HIGHEST,
};

use audio_capture::{AudioCapture, CaptureResult};
use audio_packet::{AudioPacket, SpscQueue, AUDIO_QUEUE_CAPACITY};
use usb_accessory::{find_usb_device_ids, UsbAccessoryLink, UsbDeviceId};
use util::SendHandle;

static APP_RUNNING: AtomicBool = AtomicBool::new(true);
static SESSION_RUNNING: AtomicBool = AtomicBool::new(false);

const RECONNECT_INTERVAL: Duration = Duration::from_millis(250);
const REJECTED_RETRY_INTERVAL: Duration = Duration::from_millis(3000);
const CAPTURE_RETRY_INTERVAL: Duration = Duration::from_millis(500);
const CAPTURE_RETRY_LIMIT: u32 = 10;

unsafe extern "system" fn console_control(control_type: u32) -> windows::core::BOOL {
    if control_type == CTRL_C_EVENT
        || control_type == CTRL_BREAK_EVENT
        || control_type == CTRL_CLOSE_EVENT
        || control_type == CTRL_SHUTDOWN_EVENT
    {
        APP_RUNNING.store(false, Ordering::Release);
        SESSION_RUNNING.store(false, Ordering::Release);
        return true.into();
    }
    false.into()
}

fn configure_process_scheduling() {
    unsafe {
        let _ = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        let mut throttling = PROCESS_POWER_THROTTLING_STATE::default();
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        throttling.StateMask = 0;
        let _ = SetProcessInformation(
            GetCurrentProcess(),
            windows::Win32::System::Threading::ProcessPowerThrottling,
            (&throttling as *const PROCESS_POWER_THROTTLING_STATE) as *const core::ffi::c_void,
            std::mem::size_of::<PROCESS_POWER_THROTTLING_STATE>() as u32,
        );
    }
}

fn set_audio_thread_priority() -> Option<HANDLE> {
    let mut task_index = 0u32;
    if let Ok(handle) =
        unsafe { AvSetMmThreadCharacteristicsW(windows::core::w!("Pro Audio"), &mut task_index) }
    {
        if !handle.is_invalid() {
            unsafe {
                let _ = AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
            }
            return Some(handle);
        }
    }
    unsafe {
        let _ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    None
}

fn parse_timeout(value: &OsStr) -> Option<u32> {
    let value = value.to_str()?;
    let trimmed = value.trim_start_matches(|c: char| c.is_ascii_whitespace());
    let digits = trimmed.strip_prefix('+').unwrap_or(trimmed);
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    digits.parse::<u32>().ok()
}

/// Helper so closures capture the SendHandle wrapper as a whole; capturing
/// `event.0` directly would capture the raw HANDLE field, which is !Send.
fn wait_for_event(event: SendHandle, milliseconds: u32) -> windows::Win32::Foundation::WAIT_EVENT {
    unsafe { WaitForSingleObject(event.0, milliseconds) }
}

struct AutoConnectState {
    present: Vec<UsbDeviceId>,
    rejected: Vec<UsbDeviceId>,
    preferred: UsbDeviceId,
    has_preferred: bool,
    rejected_since: Instant,
}

fn try_auto_connect(
    link: &mut UsbAccessoryLink,
    running: &AtomicBool,
    state: &mut AutoConnectState,
    timeout_milliseconds: u32,
) -> bool {
    let mut present = find_usb_device_ids();
    let now = Instant::now();
    if present != state.present
        || now.duration_since(state.rejected_since) >= REJECTED_RETRY_INTERVAL
    {
        state.present = present.clone();
        state.rejected.clear();
        state.rejected_since = now;
    }
    if state.has_preferred {
        if let Some(position) = present.iter().position(|device| *device == state.preferred) {
            present.rotate_left(position);
        }
    }
    for device_id in &present {
        if !running.load(Ordering::Acquire) {
            return false;
        }
        if state.rejected.contains(device_id) {
            continue;
        }
        if !link.enter_accessory_mode(device_id.vendor_id, device_id.product_id) {
            state.rejected.push(*device_id);
            continue;
        }
        state.preferred = *device_id;
        state.has_preferred = true;
        let deadline = Instant::now() + Duration::from_millis(timeout_milliseconds as u64);
        if !link.wait_for_accessory(running, timeout_milliseconds) {
            return false;
        }
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .map(|duration| duration.as_millis() as i64)
            .unwrap_or(-1);
        return remaining > 0 && link.wait_for_receiver(running, remaining as u32);
    }
    false
}

fn main() -> std::process::ExitCode {
    unsafe {
        let _ = SetConsoleCtrlHandler(Some(console_control), true);
    }
    configure_process_scheduling();
    let mut connect_timeout_milliseconds: u32 = 5000;
    let arguments: Vec<OsString> = std::env::args_os().skip(1).collect();
    let mut argument_index = 0;
    while argument_index < arguments.len() {
        if arguments[argument_index] != "--timeout" || argument_index + 1 >= arguments.len() {
            return std::process::ExitCode::from(2);
        }
        let Some(timeout_seconds) = parse_timeout(&arguments[argument_index + 1]) else {
            return std::process::ExitCode::from(2);
        };
        if timeout_seconds == 0 || timeout_seconds > 300 {
            return std::process::ExitCode::from(2);
        }
        connect_timeout_milliseconds = timeout_seconds * 1000;
        argument_index += 2;
    }
    let mut auto_state = AutoConnectState {
        present: Vec::new(),
        rejected: Vec::new(),
        preferred: UsbDeviceId {
            vendor_id: 0,
            product_id: 0,
        },
        has_preferred: false,
        rejected_since: Instant::now(),
    };

    while APP_RUNNING.load(Ordering::Acquire) {
        let mut link = UsbAccessoryLink::default();
        let mut connected = link.try_open_accessory();
        if connected {
            connected = link.wait_for_receiver(&APP_RUNNING, 0);
        } else {
            connected = try_auto_connect(
                &mut link,
                &APP_RUNNING,
                &mut auto_state,
                connect_timeout_milliseconds,
            );
        }
        if !connected {
            link.close();
            if !APP_RUNNING.load(Ordering::Acquire) {
                break;
            }
            thread::sleep(RECONNECT_INTERVAL);
            continue;
        }

        println!("USB connected.");
        SESSION_RUNNING.store(true, Ordering::Release);
        let queue = Arc::new(SpscQueue::<AudioPacket, AUDIO_QUEUE_CAPACITY>::new());
        let queue_event = match unsafe { CreateEventW(None, false, false, None) } {
            Ok(event) => event,
            Err(_) => {
                SESSION_RUNNING.store(false, Ordering::Release);
                link.close();
                return std::process::ExitCode::from(1);
            }
        };
        let queue_event = SendHandle(queue_event);
        let link = Arc::new(Mutex::new(link));
        let sender = {
            let link = Arc::clone(&link);
            let queue = Arc::clone(&queue);
            thread::spawn(move || {
                let mmcss_handle = set_audio_thread_priority();
                let mut packet = AudioPacket::default();
                while SESSION_RUNNING.load(Ordering::Acquire) {
                    if queue.pop(&mut packet) {
                        let mut guard = link.lock().unwrap();
                        if !guard.write_audio(&packet.samples[..packet.count as usize]) {
                            SESSION_RUNNING.store(false, Ordering::Release);
                            break;
                        }
                    } else {
                        let _ = wait_for_event(queue_event, INFINITE);
                    }
                }
                if let Some(handle) = mmcss_handle {
                    unsafe {
                        let _ = AvRevertMmThreadCharacteristics(handle);
                    }
                }
            })
        };
        let mut capture_gave_up = false;
        let mut consecutive_failures = 0u32;
        while SESSION_RUNNING.load(Ordering::Acquire) {
            let mut capture = AudioCapture::new();
            let result = capture.run(&SESSION_RUNNING, &queue, queue_event.0);
            match result {
                CaptureResult::Stopped => break,
                CaptureResult::DeviceChanged => {
                    consecutive_failures = 0;
                    continue;
                }
                CaptureResult::Failed => {
                    consecutive_failures += 1;
                    if consecutive_failures >= CAPTURE_RETRY_LIMIT {
                        capture_gave_up = true;
                        break;
                    }
                    thread::sleep(CAPTURE_RETRY_INTERVAL);
                }
            }
        }
        SESSION_RUNNING.store(false, Ordering::Release);
        unsafe {
            let _ = SetEvent(queue_event.0);
        }
        {
            let mut guard = link.lock().unwrap();
            guard.cancel_transfers();
        }
        let _ = sender.join();
        {
            let mut guard = link.lock().unwrap();
            guard.close();
        }
        unsafe {
            let _ = CloseHandle(queue_event.0);
        }
        if capture_gave_up {
            return std::process::ExitCode::from(1);
        }
    }
    std::process::ExitCode::SUCCESS
}
