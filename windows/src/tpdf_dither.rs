//! Mirrors windows/src/TpdfDither.h. xorshift32 PRNG producing a
//! triangular (TPDF) dither in [-1, 1).

pub struct TpdfDither {
    state: u32,
}

impl TpdfDither {
    pub fn new() -> Self {
        Self { state: 0x9E37_79B9 }
    }

    pub fn next(&mut self) -> f32 {
        self.next_uniform() + self.next_uniform()
    }

    fn next_uniform(&mut self) -> f32 {
        self.state ^= self.state << 13;
        self.state ^= self.state >> 17;
        self.state ^= self.state << 5;
        (self.state >> 8) as f32 * (1.0 / 16_777_216.0) - 0.5
    }
}

#[cfg(test)]
mod tests {
    use super::TpdfDither;

    #[test]
    fn matches_reference_sequence() {
        // Reference values produced by the C++ implementation.
        let mut dither = TpdfDither::new();
        let mut sequence = Vec::new();
        for _ in 0..8 {
            sequence.push(dither.next());
        }
        let expected = [
            0.192_300_5_f32,
            -0.510_784_7,
            0.873_981_4,
            0.715_348_6,
            0.367_679_66,
            0.076_680_24,
            -0.293_138_92,
            -0.687_595_96,
        ];
        for (actual, expected) in sequence.iter().zip(expected) {
            assert!(
                (actual - expected).abs() < 1e-6,
                "{} != {}",
                actual,
                expected
            );
        }
    }
}
