//! Minimal FFI bindings for the AAudio C API (aaudio/AAudio.h), mirroring
//! the calls made by the C++ Receiver. All constants are part of the
//! stable AAudio NDK ABI.

use std::ffi::{c_int, c_void};

#[allow(non_camel_case_types)]
pub type aaudio_result_t = i32;
#[allow(non_camel_case_types)]
pub type aaudio_data_callback_result_t = i32;

#[repr(C)]
pub struct AAudioStreamBuilder {
    _private: [u8; 0],
}

#[repr(C)]
pub struct AAudioStream {
    _private: [u8; 0],
}

pub const AAUDIO_OK: aaudio_result_t = 0;
pub const AAUDIO_CALLBACK_RESULT_CONTINUE: aaudio_data_callback_result_t = 0;
pub const AAUDIO_DIRECTION_OUTPUT: c_int = 0;
pub const AAUDIO_FORMAT_PCM_I16: c_int = 1;
pub const AAUDIO_PERFORMANCE_MODE_LOW_LATENCY: c_int = 12;
pub const AAUDIO_SHARING_MODE_EXCLUSIVE: c_int = 0;
pub const AAUDIO_USAGE_GAME: c_int = 14;
pub const AAUDIO_CONTENT_TYPE_MUSIC: c_int = 2;

pub type AAudioStreamDataCallback = Option<
    unsafe extern "C" fn(
        stream: *mut AAudioStream,
        user_data: *mut c_void,
        audio_data: *mut c_void,
        num_frames: c_int,
    ) -> aaudio_data_callback_result_t,
>;

#[link(name = "aaudio")]
extern "C" {
    pub fn AAudio_createStreamBuilder(builder: *mut *mut AAudioStreamBuilder) -> aaudio_result_t;
    pub fn AAudioStreamBuilder_delete(builder: *mut AAudioStreamBuilder) -> aaudio_result_t;
    pub fn AAudioStreamBuilder_setDirection(builder: *mut AAudioStreamBuilder, direction: c_int);
    pub fn AAudioStreamBuilder_setSampleRate(builder: *mut AAudioStreamBuilder, sample_rate: c_int);
    pub fn AAudioStreamBuilder_setChannelCount(builder: *mut AAudioStreamBuilder, channels: c_int);
    pub fn AAudioStreamBuilder_setFormat(builder: *mut AAudioStreamBuilder, format: c_int);
    pub fn AAudioStreamBuilder_setPerformanceMode(
        builder: *mut AAudioStreamBuilder,
        mode: c_int,
    );
    pub fn AAudioStreamBuilder_setSharingMode(builder: *mut AAudioStreamBuilder, mode: c_int);
    pub fn AAudioStreamBuilder_setUsage(builder: *mut AAudioStreamBuilder, usage: c_int);
    pub fn AAudioStreamBuilder_setContentType(
        builder: *mut AAudioStreamBuilder,
        content_type: c_int,
    );
    pub fn AAudioStreamBuilder_setDataCallback(
        builder: *mut AAudioStreamBuilder,
        callback: AAudioStreamDataCallback,
        user_data: *mut c_void,
    );
    pub fn AAudioStreamBuilder_openStream(
        builder: *mut AAudioStreamBuilder,
        stream: *mut *mut AAudioStream,
    ) -> aaudio_result_t;
    pub fn AAudioStream_getFramesPerBurst(stream: *mut AAudioStream) -> c_int;
    pub fn AAudioStream_setBufferSizeInFrames(stream: *mut AAudioStream, frames: c_int)
        -> aaudio_result_t;
    pub fn AAudioStream_requestStart(stream: *mut AAudioStream) -> aaudio_result_t;
    pub fn AAudioStream_requestStop(stream: *mut AAudioStream) -> aaudio_result_t;
    pub fn AAudioStream_close(stream: *mut AAudioStream) -> aaudio_result_t;
}
