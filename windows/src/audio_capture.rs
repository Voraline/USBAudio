//! Mirrors windows/src/AudioCapture.cpp: WASAPI loopback capture, downmix
//! to mono, linear resample to 48 kHz, and TPDF-dithered s16 conversion.

use std::ptr::null_mut;
use std::sync::atomic::{AtomicBool, Ordering};

use windows::Win32::Foundation::{
    CloseHandle, HANDLE, RPC_E_CHANGED_MODE, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows::Win32::Media::Audio::{
    eConsole, eRender, AUDCLNT_BUFFERFLAGS_SILENT, AUDCLNT_E_DEVICE_INVALIDATED,
    AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, AUDCLNT_STREAMFLAGS_LOOPBACK,
    IAudioCaptureClient, IAudioClient, IMMDevice, IMMDeviceEnumerator, MMDeviceEnumerator,
    WAVEFORMATEX, WAVEFORMATEXTENSIBLE,
};
use windows::Win32::System::Com::{
    CoCreateInstance, CoInitializeEx, CoTaskMemFree, CoUninitialize, CLSCTX_ALL,
    COINIT_MULTITHREADED,
};
use windows::Win32::System::Threading::{
    AvRevertMmThreadCharacteristics, AvSetMmThreadCharacteristicsW, AvSetMmThreadPriority,
    CreateEventW, SetEvent, WaitForMultipleObjects, AVRT_PRIORITY_HIGH,
};
use windows::core::GUID;

use usb_audio_shared::SAMPLE_RATE;

use crate::audio_packet::{AudioPacket, SpscQueue, AUDIO_QUEUE_CAPACITY};
use crate::device_watcher::DefaultDeviceWatcher;
use crate::low_latency_render::LowLatencyRender;
use crate::tpdf_dither::TpdfDither;

const WAVE_FORMAT_IEEE_FLOAT: u16 = 3;
const WAVE_FORMAT_EXTENSIBLE: u16 = 0xFFFE;
const IEEE_FLOAT_SUBFORMAT: GUID = GUID::from_u128(0x0000_0003_0000_0010_8000_00AA00389B71);

const MONO_SCRATCH_CAPACITY: usize = 4096;
const WAIT_TIMEOUT_MS: u32 = 100;
const WAIT_HANDLE_CAPACITY: usize = 3;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum CaptureResult {
    Stopped,
    DeviceChanged,
    Failed,
}

struct SourceFormat {
    sample_rate: u32,
    block_align: u32,
    bits: u16,
    channels: u16,
    is_float: bool,
}

fn read_sample(data: &[u8], bits: u16, is_float: bool) -> f32 {
    if is_float && bits == 32 {
        let value = f32::from_bits(u32::from_le_bytes([data[0], data[1], data[2], data[3]]));
        return if value.is_finite() { value } else { 0.0 };
    }
    if bits == 16 {
        let value = i16::from_le_bytes([data[0], data[1]]);
        return value as f32 / 32768.0;
    }
    if bits == 24 {
        let mut value = (data[0] as i32) | ((data[1] as i32) << 8) | ((data[2] as i32) << 16);
        if value & 0x800000 != 0 {
            value |= -0x1000000;
        }
        return value as f32 / 8388608.0;
    }
    if bits == 32 {
        let value = i32::from_le_bytes([data[0], data[1], data[2], data[3]]);
        return value as f32 / 2147483648.0;
    }
    0.0
}

fn is_float_format(format: *const WAVEFORMATEX) -> bool {
    // SAFETY: GetMixFormat returns a buffer of at least
    // sizeof(WAVEFORMATEX) + cbSize bytes.
    unsafe {
        if (*format).wFormatTag == WAVE_FORMAT_IEEE_FLOAT {
            return true;
        }
        if (*format).wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && (*format).cbSize
                >= (std::mem::size_of::<WAVEFORMATEXTENSIBLE>() - std::mem::size_of::<WAVEFORMATEX>())
                    as u16
        {
            let sub_format = std::ptr::addr_of!((*(format as *const WAVEFORMATEXTENSIBLE)).SubFormat)
                .read_unaligned();
            return sub_format == IEEE_FLOAT_SUBFORMAT;
        }
        false
    }
}

/// Owns the CoTaskMem-allocated mix format for the lifetime of the capture
/// session; WASAPI reads the full WAVEFORMATEX + cbSize bytes, so the
/// allocation must outlive every API call that receives the pointer.
struct MixFormatPtr(*mut WAVEFORMATEX);

impl Drop for MixFormatPtr {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe {
                CoTaskMemFree(Some(self.0 as *const core::ffi::c_void));
            }
        }
    }
}

fn downmix_mono_scalar(
    bytes: &[u8],
    frames: u32,
    block_align: u32,
    bits: u16,
    is_float: bool,
    channels: u16,
    mono: &mut [f32],
) {
    let bytes_per_sample = (bits / 8) as usize;
    for frame in 0..frames as usize {
        let frame_data = &bytes[frame * block_align as usize..];
        let left = read_sample(frame_data, bits, is_float);
        let right = if channels > 1 {
            read_sample(&frame_data[bytes_per_sample..], bits, is_float)
        } else {
            left
        };
        mono[frame] = (left + right) * 0.5;
    }
}

pub struct AudioCapture {
    format: SourceFormat,
    packet: AudioPacket,
    dither: TpdfDither,
    resample_phase: f64,
    previous_sample: f32,
    has_previous: bool,
    queue_event: HANDLE,
    mono_scratch: Vec<f32>,
}

impl AudioCapture {
    pub fn new() -> Self {
        Self {
            format: SourceFormat {
                sample_rate: 0,
                block_align: 0,
                bits: 0,
                channels: 0,
                is_float: false,
            },
            packet: AudioPacket::default(),
            dither: TpdfDither::new(),
            resample_phase: 0.0,
            previous_sample: 0.0,
            has_previous: false,
            queue_event: HANDLE(null_mut()),
            mono_scratch: vec![0.0f32; MONO_SCRATCH_CAPACITY],
        }
    }

    pub fn run(
        &mut self,
        running: &AtomicBool,
        queue: &SpscQueue<AudioPacket, AUDIO_QUEUE_CAPACITY>,
        queue_event_handle: HANDLE,
    ) -> CaptureResult {
        self.queue_event = queue_event_handle;
        let mut should_uninitialize = false;
        let initialize_result = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
        if initialize_result.is_ok() {
            should_uninitialize = true;
        } else if initialize_result != RPC_E_CHANGED_MODE {
            return CaptureResult::Failed;
        }

        let mut task_index = 0u32;
        let mut mmcss_handle: Option<HANDLE> = None;
        if let Ok(handle) =
            unsafe { AvSetMmThreadCharacteristicsW(windows::core::w!("Pro Audio"), &mut task_index) }
        {
            if !handle.is_invalid() {
                unsafe {
                    let _ = AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
                }
                mmcss_handle = Some(handle);
            }
        }

        let outcome = self.capture_session(running, queue);

        if let Some(handle) = mmcss_handle {
            unsafe {
                let _ = AvRevertMmThreadCharacteristics(handle);
            }
        }
        if should_uninitialize {
            unsafe {
                CoUninitialize();
            }
        }
        outcome
    }

    fn capture_session(
        &mut self,
        running: &AtomicBool,
        queue: &SpscQueue<AudioPacket, AUDIO_QUEUE_CAPACITY>,
    ) -> CaptureResult {
        let enumerator: IMMDeviceEnumerator =
            match unsafe { CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL) } {
                Ok(enumerator) => enumerator,
                Err(_) => return CaptureResult::Failed,
            };
        let watcher = match DefaultDeviceWatcher::start(&enumerator) {
            Ok(watcher) => Some(watcher),
            Err(_) => None,
        };
        let device: IMMDevice =
            match unsafe { enumerator.GetDefaultAudioEndpoint(eRender, eConsole) } {
                Ok(device) => device,
                Err(_) => {
                    if let Some(watcher) = &watcher {
                        DefaultDeviceWatcher::stop(watcher, &enumerator);
                    }
                    return CaptureResult::Failed;
                }
            };
        let client: IAudioClient =
            match unsafe { device.Activate::<IAudioClient>(CLSCTX_ALL, None) } {
                Ok(client) => client,
                Err(_) => {
                    if let Some(watcher) = &watcher {
                        DefaultDeviceWatcher::stop(watcher, &enumerator);
                    }
                    return CaptureResult::Failed;
                }
            };
        let mix_format_ptr = MixFormatPtr(match unsafe { client.GetMixFormat() } {
            Ok(format) => format,
            Err(_) => {
                if let Some(watcher) = &watcher {
                    DefaultDeviceWatcher::stop(watcher, &enumerator);
                }
                return CaptureResult::Failed;
            }
        });
        // The 18-byte prefix is copied for field access only; all API calls
        // below receive the full allocation via the raw pointer.
        let mix_format = unsafe { *mix_format_ptr.0 };
        if mix_format.nChannels == 0
            || mix_format.nSamplesPerSec == 0
            || mix_format.nBlockAlign == 0
        {
            if let Some(watcher) = &watcher {
                DefaultDeviceWatcher::stop(watcher, &enumerator);
            }
            return CaptureResult::Failed;
        }
        self.format = SourceFormat {
            sample_rate: mix_format.nSamplesPerSec,
            block_align: mix_format.nBlockAlign as u32,
            bits: mix_format.wBitsPerSample,
            channels: mix_format.nChannels,
            is_float: is_float_format(mix_format_ptr.0),
        };
        if (!self.format.is_float
            && self.format.bits != 16
            && self.format.bits != 24
            && self.format.bits != 32)
            || (self.format.is_float && self.format.bits != 32)
        {
            if let Some(watcher) = &watcher {
                DefaultDeviceWatcher::stop(watcher, &enumerator);
            }
            return CaptureResult::Failed;
        }

        let mut low_latency = LowLatencyRender::new();
        if low_latency.open(&device, mix_format_ptr.0) {
            println!("Capture mode: low latency");
        } else {
            println!("Capture mode: default");
        }

        let audio_event = match unsafe { CreateEventW(None, false, false, None) } {
            Ok(event) => event,
            Err(_) => {
                if let Some(watcher) = &watcher {
                    DefaultDeviceWatcher::stop(watcher, &enumerator);
                }
                return CaptureResult::Failed;
            }
        };
        let capture: IAudioCaptureClient = {
            let mut capture: Option<IAudioCaptureClient> = None;
            unsafe {
                let initialize = client.Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    0,
                    0,
                    mix_format_ptr.0,
                    None,
                );
                if let Err(error) = initialize {
                    eprintln!("Capture init: Initialize failed: {error}");
                } else if let Err(error) = client.SetEventHandle(audio_event) {
                    eprintln!("Capture init: SetEventHandle failed: {error}");
                } else {
                    match client.GetService::<IAudioCaptureClient>() {
                        Ok(service) => capture = Some(service),
                        Err(error) => eprintln!("Capture init: GetService failed: {error}"),
                    }
                }
                if capture.is_some() {
                    if let Err(error) = client.Start() {
                        eprintln!("Capture init: Start failed: {error}");
                        capture = None;
                    }
                }
            }
            match capture {
                Some(capture) => capture,
                None => {
                    unsafe {
                        let _ = client.Stop();
                    }
                    if let Some(watcher) = &watcher {
                        DefaultDeviceWatcher::stop(watcher, &enumerator);
                    }
                    unsafe {
                        let _ = CloseHandle(audio_event);
                    }
                    return CaptureResult::Failed;
                }
            }
        };

        let mut wait_handles: Vec<HANDLE> = Vec::with_capacity(WAIT_HANDLE_CAPACITY);
        wait_handles.push(audio_event);
        let device_index = if let Some(watcher) = &watcher {
            let index = wait_handles.len() as u32;
            wait_handles.push(watcher.get_event());
            Some(index)
        } else {
            None
        };
        let render_index = match low_latency.get_event() {
            Some(event) => {
                let index = wait_handles.len() as u32;
                wait_handles.push(event);
                Some(index)
            }
            None => None,
        };

        let mut outcome = CaptureResult::Stopped;
        while running.load(Ordering::Acquire) {
            let wait_result =
                unsafe { WaitForMultipleObjects(&wait_handles, false, WAIT_TIMEOUT_MS) };
            if wait_result == WAIT_TIMEOUT {
                if unsafe { capture.GetNextPacketSize() }.is_err() {
                    outcome = CaptureResult::DeviceChanged;
                    break;
                }
                continue;
            }
            if wait_result.0 >= WAIT_OBJECT_0.0 + wait_handles.len() as u32 {
                running.store(false, Ordering::Release);
                break;
            }
            let signaled = wait_result.0 - WAIT_OBJECT_0.0;
            if Some(signaled) == device_index {
                outcome = CaptureResult::DeviceChanged;
                break;
            }
            if Some(signaled) == render_index {
                low_latency.fill_silence();
                continue;
            }
            if signaled != 0 {
                continue;
            }
            let drain_result = self.drain_packets(&capture, running, queue);
            self.flush_packet(queue);
            if let Err(error) = drain_result {
                if error.code() == AUDCLNT_E_DEVICE_INVALIDATED {
                    outcome = CaptureResult::DeviceChanged;
                } else {
                    running.store(false, Ordering::Release);
                }
                break;
            }
        }

        unsafe {
            let _ = client.Stop();
        }
        if let Some(watcher) = &watcher {
            DefaultDeviceWatcher::stop(watcher, &enumerator);
        }
        unsafe {
            let _ = CloseHandle(audio_event);
        }
        outcome
    }

    fn drain_packets(
        &mut self,
        capture: &IAudioCaptureClient,
        running: &AtomicBool,
        queue: &SpscQueue<AudioPacket, AUDIO_QUEUE_CAPACITY>,
    ) -> windows::core::Result<()> {
        loop {
            let packet_frames = unsafe { capture.GetNextPacketSize() }?;
            if packet_frames == 0 || !running.load(Ordering::Acquire) {
                return Ok(());
            }
            let mut data: *mut u8 = null_mut();
            let mut frames = 0u32;
            let mut flags = 0u32;
            unsafe { capture.GetBuffer(&mut data, &mut frames, &mut flags, None, None) }?;
            // SAFETY: WASAPI guarantees frames * block_align valid bytes.
            let bytes = unsafe {
                std::slice::from_raw_parts(data, (frames as usize) * self.format.block_align as usize)
            };
            self.convert_buffer(bytes, frames, flags, queue);
            unsafe {
                let _ = capture.ReleaseBuffer(frames);
            }
        }
    }

    fn convert_buffer(
        &mut self,
        data: &[u8],
        frames: u32,
        flags: u32,
        queue: &SpscQueue<AudioPacket, AUDIO_QUEUE_CAPACITY>,
    ) {
        let silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT.0 as u32) != 0;
        let mut frames_done = 0u32;
        while frames_done < frames {
            let chunk_frames = (frames - frames_done).min(MONO_SCRATCH_CAPACITY as u32) as usize;
            if silent {
                for sample in &mut self.mono_scratch[..chunk_frames] {
                    *sample = 0.0;
                }
            } else {
                let chunk_bytes =
                    &data[(frames_done as usize) * self.format.block_align as usize..];
                downmix_mono_scalar(
                    chunk_bytes,
                    chunk_frames as u32,
                    self.format.block_align,
                    self.format.bits,
                    self.format.is_float,
                    self.format.channels,
                    &mut self.mono_scratch[..chunk_frames],
                );
            }
            for frame in 0..chunk_frames {
                self.convert_frame(self.mono_scratch[frame], queue);
            }
            frames_done += chunk_frames as u32;
        }
    }

    fn convert_frame(&mut self, mono: f32, queue: &SpscQueue<AudioPacket, AUDIO_QUEUE_CAPACITY>) {
        if self.format.sample_rate == SAMPLE_RATE {
            let sample = self.to_pcm16(mono);
            self.append_sample(sample, queue);
            return;
        }
        let step = self.format.sample_rate as f64 / SAMPLE_RATE as f64;
        if !self.has_previous {
            self.previous_sample = mono;
            self.has_previous = true;
            let sample = self.to_pcm16(mono);
            self.append_sample(sample, queue);
            self.resample_phase = step;
            return;
        }
        while self.resample_phase <= 1.0 {
            let ratio = self.resample_phase.clamp(0.0, 1.0) as f32;
            let sample = self.to_pcm16(self.previous_sample + (mono - self.previous_sample) * ratio);
            self.append_sample(sample, queue);
            self.resample_phase += step;
        }
        self.resample_phase -= 1.0;
        self.previous_sample = mono;
    }

    fn append_sample(
        &mut self,
        sample: i16,
        queue: &SpscQueue<AudioPacket, AUDIO_QUEUE_CAPACITY>,
    ) {
        self.packet.samples[self.packet.count as usize] = sample;
        self.packet.count += 1;
        if self.packet.count as usize == self.packet.samples.len() {
            self.flush_packet(queue);
        }
    }

    fn flush_packet(&mut self, queue: &SpscQueue<AudioPacket, AUDIO_QUEUE_CAPACITY>) {
        if self.packet.count == 0 {
            return;
        }
        if queue.push(self.packet) && !self.queue_event.is_invalid() {
            unsafe {
                let _ = SetEvent(self.queue_event);
            }
        }
        self.packet.count = 0;
    }

    fn to_pcm16(&mut self, value: f32) -> i16 {
        if value == 0.0 {
            return 0;
        }
        let scaled = value.clamp(-1.0, 1.0) * 32767.0 + self.dither.next();
        let rounded = scaled.round_ties_even();
        (rounded as i64).clamp(-32768, 32767) as i16
    }
}

#[cfg(test)]
mod tests {
    use super::{downmix_mono_scalar, read_sample};

    #[test]
    fn read_i16_sample() {
        let bytes = (-12345i16).to_le_bytes();
        let expected = -12345.0f32 / 32768.0;
        assert_eq!(read_sample(&bytes, 16, false), expected);
    }

    #[test]
    fn read_i24_sample_sign_extends() {
        // 0x800001 as a sign-extended i24 is -8388607.
        let bytes = [0x01u8, 0x00, 0x80];
        let expected = -8388607.0f32 / 8388608.0f32;
        assert!((read_sample(&bytes, 24, false) - expected).abs() < 1e-6);
        // 0x800000 as a sign-extended i24 is -8388608.
        let bytes = [0x00u8, 0x00, 0x80];
        assert_eq!(read_sample(&bytes, 24, false), -1.0);
    }

    #[test]
    fn read_i32_sample() {
        let bytes = 1073741824i32.to_le_bytes();
        let expected = 1073741824.0f32 / 2147483648.0;
        assert_eq!(read_sample(&bytes, 32, false), expected);
    }

    #[test]
    fn read_f32_non_finite_is_zero() {
        let bytes = f32::INFINITY.to_le_bytes();
        assert_eq!(read_sample(&bytes, 32, true), 0.0);
        let bytes = f32::NAN.to_le_bytes();
        assert_eq!(read_sample(&bytes, 32, true), 0.0);
    }

    #[test]
    fn downmix_stereo_averages() {
        let left = 0.25f32;
        let right = 0.75f32;
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&left.to_le_bytes());
        bytes.extend_from_slice(&right.to_le_bytes());
        let mut mono = [0.0f32; 2];
        downmix_mono_scalar(&bytes, 1, 8, 32, true, 2, &mut mono);
        assert_eq!(mono[0], 0.5);
    }

    #[test]
    fn downmix_mono_passes_through() {
        let sample = -0.5f32;
        let bytes = sample.to_le_bytes();
        let mut mono = [0.0f32; 1];
        downmix_mono_scalar(&bytes, 1, 4, 32, true, 1, &mut mono);
        assert_eq!(mono[0], sample);
    }
}
