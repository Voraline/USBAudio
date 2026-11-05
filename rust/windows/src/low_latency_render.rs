//! Mirrors windows/src/LowLatencyRender.cpp: a low-latency silent render
//! client that keeps the shared-mode WASAPI engine primed at its minimum
//! period so loopback capture runs with minimal latency.

use windows::Win32::Foundation::{CloseHandle, HANDLE};
use windows::Win32::Media::Audio::{
    AUDCLNT_BUFFERFLAGS_SILENT, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, IAudioClient3,
    IAudioRenderClient, IMMDevice, WAVEFORMATEX,
};
use windows::Win32::System::Com::CLSCTX_ALL;
use windows::Win32::System::Threading::CreateEventW;

pub struct LowLatencyRender {
    client: Option<IAudioClient3>,
    render: Option<IAudioRenderClient>,
    render_event: Option<HANDLE>,
    buffer_frames: u32,
}

impl LowLatencyRender {
    pub fn new() -> Self {
        Self {
            client: None,
            render: None,
            render_event: None,
            buffer_frames: 0,
        }
    }

    pub fn open(&mut self, device: &IMMDevice, format: &WAVEFORMATEX) -> bool {
        self.close();
        let mut default_period = 0u32;
        let mut fundamental_period = 0u32;
        let mut minimum_period = 0u32;
        let mut maximum_period = 0u32;
        let render_event = match unsafe { CreateEventW(None, false, false, None) } {
            Ok(event) => event,
            Err(_) => return false,
        };
        let client: IAudioClient3 =
            match unsafe { device.Activate::<IAudioClient3>(CLSCTX_ALL, None) } {
                Ok(client) => client,
                Err(_) => {
                    unsafe {
                        let _ = CloseHandle(render_event);
                    }
                    return false;
                }
            };
        unsafe {
            if client
                .GetSharedModeEnginePeriod(
                    format,
                    &mut default_period,
                    &mut fundamental_period,
                    &mut minimum_period,
                    &mut maximum_period,
                )
                .is_err()
                || minimum_period >= default_period
                || client
                    .InitializeSharedAudioStream(
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        minimum_period,
                        format,
                        None,
                    )
                    .is_err()
                || client.SetEventHandle(render_event).is_err()
            {
                return false;
            }
        }
        let buffer_frames = match unsafe { client.GetBufferSize() } {
            Ok(frames) => frames,
            Err(_) => return false,
        };
        let render: IAudioRenderClient = match unsafe { client.GetService::<IAudioRenderClient>() } {
            Ok(render) => render,
            Err(_) => return false,
        };
        self.client = Some(client);
        self.render = Some(render);
        self.render_event = Some(render_event);
        self.buffer_frames = buffer_frames;
        self.fill_silence();
        if unsafe { self.client.as_ref().unwrap().Start() }.is_err() {
            self.close();
            return false;
        }
        true
    }

    pub fn fill_silence(&mut self) {
        let (client, render) = match (self.client.as_ref(), self.render.as_ref()) {
            (Some(client), Some(render)) => (client, render),
            _ => return,
        };
        let padding = match unsafe { client.GetCurrentPadding() } {
            Ok(padding) => padding,
            Err(_) => return,
        };
        if padding >= self.buffer_frames {
            return;
        }
        let available = self.buffer_frames - padding;
        if let Ok(_data) = unsafe { render.GetBuffer(available) } {
            unsafe {
                let _ = render.ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT.0 as u32);
            }
        }
    }

    pub fn close(&mut self) {
        if let Some(client) = self.client.as_ref() {
            unsafe {
                let _ = client.Stop();
            }
        }
        self.render = None;
        self.client = None;
        if let Some(event) = self.render_event.take() {
            unsafe {
                let _ = CloseHandle(event);
            }
        }
        self.buffer_frames = 0;
    }

    pub fn get_event(&self) -> Option<HANDLE> {
        self.render_event
    }
}

impl Drop for LowLatencyRender {
    fn drop(&mut self) {
        self.close();
    }
}
