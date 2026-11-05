//! PCM format shared by the Windows sender and the Android receiver.
//! Mirrors shared/AudioFormat.h.

pub const RECEIVER_READY: u8 = 1;
pub const MAX_PACKET_SAMPLES: usize = 480;
pub const SAMPLE_RATE: u32 = 48_000;
pub const CHANNELS: u16 = 1;
