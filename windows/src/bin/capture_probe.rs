//! Diagnostic probe: runs the WASAPI capture session while audio plays,
//! collecting converted samples to a raw i16 LE mono 48 kHz file. Not
//! shipped in the release executable.

#[path = "../audio_packet.rs"]
mod audio_packet;
#[path = "../device_watcher.rs"]
mod device_watcher;
#[path = "../low_latency_render.rs"]
mod low_latency_render;
#[path = "../tpdf_dither.rs"]
mod tpdf_dither;
#[path = "../util.rs"]
#[allow(dead_code)]
mod util;
#[path = "../audio_capture.rs"]
mod audio_capture;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use windows::Win32::Foundation::HANDLE;
use windows::Win32::System::Threading::CreateEventW;

use audio_capture::{AudioCapture, CaptureResult};
use audio_packet::{AudioPacket, SpscQueue, AUDIO_QUEUE_CAPACITY};

fn main() {
    let out_path = std::env::args().nth(1).unwrap_or_else(|| "captured.raw".into());
    let running = Arc::new(AtomicBool::new(true));
    let stop = Arc::new(AtomicBool::new(false));
    let queue = Arc::new(SpscQueue::<AudioPacket, AUDIO_QUEUE_CAPACITY>::new());
    let event: HANDLE = match unsafe { CreateEventW(None, false, false, None) } {
        Ok(event) => event,
        Err(error) => {
            eprintln!("CreateEventW failed: {error}");
            return;
        }
    };
    let collector = {
        let queue = Arc::clone(&queue);
        let stop = Arc::clone(&stop);
        std::thread::spawn(move || {
            let mut samples: Vec<i16> = Vec::new();
            let mut packet = AudioPacket::default();
            loop {
                if queue.pop(&mut packet) {
                    samples.extend_from_slice(&packet.samples[..packet.count as usize]);
                } else if stop.load(Ordering::Acquire) {
                    break;
                } else {
                    std::thread::sleep(std::time::Duration::from_millis(1));
                }
            }
            samples
        })
    };
    let timer = {
        let running = Arc::clone(&running);
        std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_secs(4));
            running.store(false, Ordering::Release);
        })
    };
    let mut capture = AudioCapture::new();
    let start = std::time::Instant::now();
    let result = capture.run(&running, &queue, event);
    let _ = timer.join();
    stop.store(true, Ordering::Release);
    let samples = collector.join().unwrap();
    let (rate, align, bits, channels, is_float) = capture.source_format();
    println!(
        "mix format: rate={rate} block_align={align} bits={bits} channels={channels} is_float={is_float}"
    );
    println!(
        "elapsed: {:?}, result: {:?}, samples: {} ({:.2} s at 48 kHz)",
        start.elapsed(),
        match result {
            CaptureResult::Stopped => "Stopped",
            CaptureResult::DeviceChanged => "DeviceChanged",
            CaptureResult::Failed => "Failed",
        },
        samples.len(),
        samples.len() as f64 / 48000.0
    );
    let bytes: Vec<u8> = samples
        .iter()
        .flat_map(|sample| sample.to_le_bytes())
        .collect();
    std::fs::write(&out_path, bytes).expect("failed to write captured.raw");
    println!("wrote {out_path}");
}
