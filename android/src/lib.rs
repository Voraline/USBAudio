//! USB audio receiver native library (libusb_audio_native.so).
//! Platform-independent DSP lives in `dsp`; the Android-specific receiver,
//! AAudio FFI, and JNI bridge are compiled only for Android targets.

pub mod dsp;

#[cfg(target_os = "android")]
mod android;

#[cfg(test)]
mod tests {
    use crate::dsp::{InterpolationFilter, PcmRing};

    #[test]
    fn ring_writes_and_wraps() {
        let ring = PcmRing::new();
        let samples: Vec<i16> = (0..4096).map(|i| (i % 1000) as i16).collect();
        assert!(ring.write(&samples[..2048]));
        assert!(!ring.write(&samples[..1])); // full
        assert_eq!(ring.get_write_index(), 2048);
        assert_eq!(ring.get_read_index(), 0);
        ring.publish_read_index(1024);
        assert!(ring.write(&samples[2048..3072]));
        assert_eq!(ring.get_write_index(), 3072);
        // Read-back matches written samples including wrap-around.
        assert_eq!(ring.get_sample(1024), samples[1024]);
        assert_eq!(ring.get_sample(3071), samples[3071]);
    }

    #[test]
    fn filter_table_is_normalized_and_centered() {
        let filter = InterpolationFilter::new();
        for row in 0..=InterpolationFilter::PHASES {
            let sum: f32 = (0..InterpolationFilter::TAPS)
                .map(|tap| filter.table_value(row, tap))
                .sum();
            assert!(
                (sum - 1.0).abs() < 1e-4,
                "row {} sums to {}",
                row,
                sum
            );
        }
    }

    #[test]
    fn filter_applies_interpolation() {
        let filter = InterpolationFilter::new();
        let ring = PcmRing::new();
        // Constant signal reproduces exactly.
        let constant: Vec<i16> = vec![1000; 2048];
        assert!(ring.write(&constant));
        let base = 100u64;
        let value = filter.apply(&ring, base, 0.3);
        assert!((value - 1000.0).abs() < 1.0);
        // Impulse response at the right offset: with the impulse at index
        // 100 and base = 100, the center tap (tap 7, reading base) lands
        // exactly on it and phase 0 passes it through at full weight.
        let value = filter.apply(&ring, 100, 0.0);
        assert!((value - 1000.0).abs() < 0.01, "value = {}", value);
    }
}
