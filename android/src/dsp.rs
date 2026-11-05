//! Mirrors the PcmRing and InterpolationFilter classes from
//! android/app/src/main/cpp/Receiver.cpp. Pure Rust, no platform APIs, so
//! it can be unit-tested on the host.

use std::sync::atomic::{AtomicU64, Ordering};

pub struct PcmRing {
    samples: [i16; PcmRing::CAPACITY],
    write_index: AlignedAtomicU64,
    read_index: AlignedAtomicU64,
}

// SAFETY: single-writer (transport thread) / single-reader (AAudio callback)
// discipline enforced by the atomic indices, exactly like the C++ version.
unsafe impl Sync for PcmRing {}

#[repr(align(64))]
struct AlignedAtomicU64(AtomicU64);

impl PcmRing {
    pub const CAPACITY_FRAMES: u64 = 2048;
    const CAPACITY: usize = 2048;

    pub fn new() -> Self {
        Self {
            samples: [0i16; Self::CAPACITY],
            write_index: AlignedAtomicU64(AtomicU64::new(0)),
            read_index: AlignedAtomicU64(AtomicU64::new(0)),
        }
    }

    pub fn write(&self, samples: &[i16]) -> bool {
        let write_frame = self.write_index.0.load(Ordering::Relaxed);
        let read_frame = self.read_index.0.load(Ordering::Acquire);
        let queued_frames = (write_frame - read_frame).min(Self::CAPACITY_FRAMES);
        let frames = samples.len() as u64;
        if frames > Self::CAPACITY_FRAMES - queued_frames {
            return false;
        }
        // SAFETY: the transport thread is the only writer and owns
        // [write_frame, write_frame + frames), which the reader has not yet
        // consumed per the capacity check above.
        let base = self.samples.as_ptr() as *mut i16;
        for (frame, sample) in samples.iter().enumerate() {
            unsafe {
                *base.add(((write_frame + frame as u64) & (Self::CAPACITY_FRAMES - 1)) as usize) =
                    *sample;
            }
        }
        self.write_index.0.store(write_frame + frames, Ordering::Release);
        true
    }

    pub fn get_write_index(&self) -> u64 {
        self.write_index.0.load(Ordering::Acquire)
    }

    pub fn get_read_index(&self) -> u64 {
        self.read_index.0.load(Ordering::Acquire)
    }

    pub fn publish_read_index(&self, value: u64) {
        self.read_index.0.store(value, Ordering::Release);
    }

    pub fn get_sample(&self, index: u64) -> i16 {
        // SAFETY: caller only reads samples it has consumed and not yet
        // overwritten, per the SPSC discipline.
        unsafe { *self.samples.as_ptr().add((index & (Self::CAPACITY_FRAMES - 1)) as usize) }
    }
}

impl Default for PcmRing {
    fn default() -> Self {
        Self::new()
    }
}

pub struct InterpolationFilter {
    table: [[f32; Self::TAPS]; Self::PHASES + 1],
}

impl InterpolationFilter {
    pub const TAPS: usize = 16;
    pub const PHASES: usize = 256;
    pub const HISTORY_FRAMES: usize = Self::TAPS / 2 - 1;
    pub const LOOKAHEAD_FRAMES: usize = Self::TAPS / 2;

    pub fn new() -> Self {
        let mut table = [[0.0f32; Self::TAPS]; Self::PHASES + 1];
        for (row, entry) in table.iter_mut().enumerate() {
            let fraction = row as f64 / Self::PHASES as f64;
            let mut weights = [0.0f64; Self::TAPS];
            let mut sum = 0.0;
            for (tap, weight) in weights.iter_mut().enumerate() {
                let distance = tap as f64 - Self::HISTORY_FRAMES as f64 - fraction;
                *weight = Self::sinc(distance) * Self::window(distance);
                sum += *weight;
            }
            for (tap, weight) in entry.iter_mut().enumerate() {
                *weight = (weights[tap] / sum) as f32;
            }
        }
        Self { table }
    }

    pub fn table_value(&self, row: usize, tap: usize) -> f32 {
        self.table[row][tap]
    }

    pub fn apply(&self, ring: &PcmRing, base: u64, fraction: f64) -> f32 {
        let scaled = fraction * Self::PHASES as f64;
        let row = (scaled as usize).min(Self::PHASES - 1);
        let blend = (scaled - row as f64) as f32;
        let lower = &self.table[row];
        let upper = &self.table[row + 1];
        let first = base.wrapping_sub(Self::HISTORY_FRAMES as u64);
        let mut sum = 0.0f32;
        for tap in 0..Self::TAPS {
            let weight = lower[tap] + (upper[tap] - lower[tap]) * blend;
            sum += weight * ring.get_sample(first + tap as u64) as f32;
        }
        sum
    }

    fn sinc(value: f64) -> f64 {
        if value.abs() < 1.0e-12 {
            return 1.0;
        }
        let argument = std::f64::consts::PI * value;
        argument.sin() / argument
    }

    fn window(distance: f64) -> f64 {
        let angle = std::f64::consts::PI * distance / Self::LOOKAHEAD_FRAMES as f64;
        0.42 + 0.5 * angle.cos() + 0.08 * (2.0 * angle).cos()
    }
}

impl Default for InterpolationFilter {
    fn default() -> Self {
        Self::new()
    }
}
