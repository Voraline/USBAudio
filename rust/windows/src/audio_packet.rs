//! Mirrors windows/src/AudioPacket.h: fixed-capacity SPSC queue of
//! 480-sample audio packets.

use std::sync::atomic::{AtomicUsize, Ordering};

use usb_audio_shared::MAX_PACKET_SAMPLES;

pub const AUDIO_QUEUE_CAPACITY: usize = 64;

#[derive(Clone, Copy)]
pub struct AudioPacket {
    pub count: u16,
    pub samples: [i16; MAX_PACKET_SAMPLES],
}

impl Default for AudioPacket {
    fn default() -> Self {
        Self {
            count: 0,
            samples: [0i16; MAX_PACKET_SAMPLES],
        }
    }
}

#[repr(align(64))]
struct CacheLine(AtomicUsize);

pub struct SpscQueue<T, const CAPACITY: usize> {
    items: [T; CAPACITY],
    write_index: CacheLine,
    read_index: CacheLine,
}

impl<T: Copy + Default, const CAPACITY: usize> SpscQueue<T, CAPACITY> {
    pub fn new() -> Self {
        assert!(CAPACITY > 1 && CAPACITY.is_power_of_two());
        Self {
            items: [T::default(); CAPACITY],
            write_index: CacheLine(AtomicUsize::new(0)),
            read_index: CacheLine(AtomicUsize::new(0)),
        }
    }

    pub fn push(&self, value: T) -> bool {
        let write = self.write_index.0.load(Ordering::Relaxed);
        let next = (write + 1) & (CAPACITY - 1);
        if next == self.read_index.0.load(Ordering::Acquire) {
            return false;
        }
        // SAFETY: single producer owns the write slot.
        unsafe {
            let ptr = self.items.as_ptr().add(write & (CAPACITY - 1)) as *mut T;
            ptr.write(value);
        }
        self.write_index.0.store(next, Ordering::Release);
        true
    }

    pub fn pop(&self, value: &mut T) -> bool {
        let read = self.read_index.0.load(Ordering::Relaxed);
        if read == self.write_index.0.load(Ordering::Acquire) {
            return false;
        }
        // SAFETY: single consumer owns the read slot.
        unsafe {
            *value = self.items.as_ptr().add(read & (CAPACITY - 1)).read();
        }
        self.read_index.0.store((read + 1) & (CAPACITY - 1), Ordering::Release);
        true
    }
}
