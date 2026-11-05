//! Mirrors the Receiver class from android/app/src/main/cpp/Receiver.cpp:
//! accessory descriptor transport, bounded PCM ring, 16-tap windowed-sinc
//! interpolation with PI rate control, and AAudio playback.

use std::ffi::{c_char, c_int, c_void};
use std::ptr::null_mut;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use usb_audio_shared::{CHANNELS, RECEIVER_READY, SAMPLE_RATE};

use super::aaudio::{
    self, AAUDIO_CALLBACK_RESULT_CONTINUE, AAUDIO_CONTENT_TYPE_MUSIC, AAUDIO_DIRECTION_OUTPUT,
    AAUDIO_FORMAT_PCM_I16, AAUDIO_OK, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY,
    AAUDIO_SHARING_MODE_EXCLUSIVE, AAUDIO_USAGE_GAME,
};
use super::performance_hint::PerformanceHint;
use crate::dsp::{InterpolationFilter, PcmRing};

const ANDROID_LOG_INFO: c_int = 4;

#[link(name = "log")]
extern "C" {
    fn __android_log_print(priority: c_int, tag: *const c_char, fmt: *const c_char, ...) -> c_int;
}

fn log_stats(
    fill: i32,
    target: i32,
    adjust_ppm: i32,
    underruns: u32,
    resyncs: u32,
    dropped: u32,
) {
    unsafe {
        __android_log_print(
            ANDROID_LOG_INFO,
            b"UsbAudio\0".as_ptr() as *const c_char,
            b"fill=%d target=%d adjust=%dppm underruns=%u resyncs=%u dropped=%u\0".as_ptr()
                as *const c_char,
            fill,
            target,
            adjust_ppm,
            underruns,
            resyncs,
            dropped,
        );
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PlaybackState {
    Priming,
    Playing,
}

struct Playback {
    state: PlaybackState,
    position: f64,
    smoothed_error: f64,
    integral: f64,
    adjustment: f64,
    extra_target_frames: f64,
    last_value: f32,
    fade_in_progress: i32,
}

impl Default for Playback {
    fn default() -> Self {
        Self {
            state: PlaybackState::Priming,
            position: 0.0,
            smoothed_error: 0.0,
            integral: 0.0,
            adjustment: 0.0,
            extra_target_frames: 0.0,
            last_value: 0.0,
            fade_in_progress: START_FADE_FRAMES,
        }
    }
}

pub struct Receiver {
    descriptor: AtomicI32,
    stream: AtomicPtr<aaudio::AAudioStream>,
    user_data: AtomicPtr<Receiver>,
    ring: PcmRing,
    filter: InterpolationFilter,
    running: AtomicBool,
    transport_thread: Mutex<Option<JoinHandle<()>>>,
    readiness_thread: Mutex<Option<JoinHandle<()>>>,
    playback: std::cell::UnsafeCell<Playback>,
    underruns: AtomicU32,
    resyncs: AtomicU32,
    dropped_chunks: AtomicU32,
    stat_fill_frames: AtomicI32,
    stat_target_frames: AtomicI32,
    stat_adjustment_ppm: AtomicI32,
}

// SAFETY: `playback` is only mutated by the AAudio callback thread, which
// AAudio guarantees runs alone until the stream is closed; `descriptor` and
// `stream` are only mutated after the transport/readiness threads have been
// joined. This matches the thread discipline of the C++ Receiver.
unsafe impl Sync for Receiver {}

const SAMPLE_RATE_FRAMES: f64 = SAMPLE_RATE as f64;
const MINIMUM_OUTPUT_BUFFER_FRAMES: i32 = 192;
const TRANSPORT_POLL_TIMEOUT_MS: c_int = 100;
const READINESS_INTERVAL_MS: u64 = 500;
const TRANSPORT_NICE_PRIORITY: c_int = -19;
const TRANSPORT_TARGET_WORK_NANOS: i64 = 300_000;
const TRANSPORT_REPORT_INTERVAL_PACKETS: u32 = 8;
const STATS_INTERVAL_TICKS: u32 = 20;
const READ_REQUEST_BYTES: usize = 4096;
const MINIMUM_TARGET_FRAMES: f64 = 240.0;
const MAXIMUM_TARGET_FRAMES: f64 = 1024.0;
const TARGET_MARGIN_FRAMES: f64 = 128.0;
const UNDERRUN_TARGET_STEP_FRAMES: f64 = 32.0;
const MAXIMUM_EXTRA_TARGET_FRAMES: f64 = 512.0;
const RESYNC_HIGH_WATER_FRAMES: f64 = 960.0;
const RESYNC_FADE_FRAMES: i32 = 48;
const START_FADE_FRAMES: i32 = 48;
const STARVATION_DECAY: f32 = 0.9;
const ERROR_SMOOTHING_SECONDS: f64 = 0.25;
const PROPORTIONAL_GAIN: f64 = 1.25e-5;
const INTEGRAL_GAIN: f64 = 1.875e-6;
const MAXIMUM_ADJUSTMENT: f64 = 0.002;

unsafe extern "C" fn audio_callback(
    _stream: *mut aaudio::AAudioStream,
    user_data: *mut c_void,
    audio_data: *mut c_void,
    num_frames: c_int,
) -> aaudio::aaudio_data_callback_result_t {
    // SAFETY: the pointer is owned by the stream until it is closed, and
    // AAudio invokes the callback on a single thread.
    let receiver = unsafe { &*(user_data as *const Receiver) };
    receiver.render(audio_data as *mut i16, num_frames);
    AAUDIO_CALLBACK_RESULT_CONTINUE
}

fn to_pcm16(value: f32) -> i16 {
    value.clamp(-32768.0, 32767.0).round_ties_even() as i16
}

impl Receiver {
    pub fn new(accessory_descriptor: i32) -> Self {
        Self {
            descriptor: AtomicI32::new(accessory_descriptor),
            stream: AtomicPtr::new(null_mut()),
            user_data: AtomicPtr::new(null_mut()),
            ring: PcmRing::new(),
            filter: InterpolationFilter::new(),
            running: AtomicBool::new(false),
            transport_thread: Mutex::new(None),
            readiness_thread: Mutex::new(None),
            playback: std::cell::UnsafeCell::new(Playback::default()),
            underruns: AtomicU32::new(0),
            resyncs: AtomicU32::new(0),
            dropped_chunks: AtomicU32::new(0),
            stat_fill_frames: AtomicI32::new(0),
            stat_target_frames: AtomicI32::new(0),
            stat_adjustment_ppm: AtomicI32::new(0),
        }
    }

    pub fn start(self: &Arc<Self>) -> bool {
        let descriptor = self.descriptor.load(Ordering::Relaxed);
        if descriptor < 0 {
            return false;
        }
        let flags = unsafe { libc::fcntl(descriptor, libc::F_GETFL, 0) };
        let user_data = Arc::into_raw(Arc::clone(self)).cast_mut();
        self.user_data.store(user_data, Ordering::Relaxed);
        if flags < 0
            || unsafe { libc::fcntl(descriptor, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0
            || !self.open_audio_stream(user_data)
        {
            self.stop();
            return false;
        }
        self.running.store(true, Ordering::Release);
        let transport = {
            let receiver = Arc::clone(self);
            thread::spawn(move || receiver.run_transport())
        };
        *self.transport_thread.lock().unwrap() = Some(transport);
        if !self.write_ready() {
            self.stop();
            return false;
        }
        let readiness = {
            let receiver = Arc::clone(self);
            thread::spawn(move || receiver.run_readiness())
        };
        *self.readiness_thread.lock().unwrap() = Some(readiness);
        true
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::Release);
        if let Some(thread) = self.transport_thread.lock().unwrap().take() {
            let _ = thread.join();
        }
        if let Some(thread) = self.readiness_thread.lock().unwrap().take() {
            let _ = thread.join();
        }
        let stream = self.stream.swap(null_mut(), Ordering::Relaxed);
        if !stream.is_null() {
            unsafe {
                let _ = aaudio::AAudioStream_requestStop(stream);
                let _ = aaudio::AAudioStream_close(stream);
            }
        }
        let user_data = self.user_data.swap(null_mut(), Ordering::Relaxed);
        if !user_data.is_null() {
            // SAFETY: after the stream is closed no callback can run, so the
            // leaked clone is reclaimed exactly once.
            unsafe {
                drop(Arc::from_raw(user_data));
            }
        }
        let descriptor = self.descriptor.swap(-1, Ordering::Relaxed);
        if descriptor >= 0 {
            unsafe {
                libc::close(descriptor);
            }
        }
    }

    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::Acquire)
    }

    fn open_audio_stream(&self, user_data: *mut Receiver) -> bool {
        let mut builder: *mut aaudio::AAudioStreamBuilder = null_mut();
        if unsafe { aaudio::AAudio_createStreamBuilder(&mut builder) } != AAUDIO_OK
            || builder.is_null()
        {
            return false;
        }
        unsafe {
            aaudio::AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
            aaudio::AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE as c_int);
            aaudio::AAudioStreamBuilder_setChannelCount(builder, CHANNELS as c_int);
            aaudio::AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
            aaudio::AAudioStreamBuilder_setPerformanceMode(
                builder,
                AAUDIO_PERFORMANCE_MODE_LOW_LATENCY,
            );
            aaudio::AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
            aaudio::AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
            aaudio::AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
            aaudio::AAudioStreamBuilder_setDataCallback(builder, Some(audio_callback), user_data as *mut c_void);
        }
        let mut stream: *mut aaudio::AAudioStream = null_mut();
        let open_result = unsafe { aaudio::AAudioStreamBuilder_openStream(builder, &mut stream) };
        unsafe {
            let _ = aaudio::AAudioStreamBuilder_delete(builder);
        }
        if open_result != AAUDIO_OK || stream.is_null() {
            return false;
        }
        self.stream.store(stream, Ordering::Relaxed);
        let burst_frames = unsafe { aaudio::AAudioStream_getFramesPerBurst(stream) };
        if burst_frames > 0 {
            unsafe {
                let _ = aaudio::AAudioStream_setBufferSizeInFrames(
                    stream,
                    burst_frames.max(MINIMUM_OUTPUT_BUFFER_FRAMES),
                );
            }
        }
        let start_result = unsafe { aaudio::AAudioStream_requestStart(stream) };
        start_result == AAUDIO_OK
    }

    fn render(&self, output: *mut i16, num_frames: c_int) {
        if num_frames <= 0 {
            return;
        }
        let num_frames = num_frames as usize;
        // SAFETY: AAudio guarantees the buffer holds num_frames samples.
        let output = unsafe { std::slice::from_raw_parts_mut(output, num_frames) };
        // SAFETY: only the callback thread runs render.
        let playback = unsafe { &mut *self.playback.get() };
        let write_frame = self.ring.get_write_index();
        let target_frames = (num_frames as f64 + TARGET_MARGIN_FRAMES + playback.extra_target_frames)
            .clamp(MINIMUM_TARGET_FRAMES, MAXIMUM_TARGET_FRAMES);
        let mut queued = write_frame as f64 - playback.position;
        if playback.state == PlaybackState::Priming {
            if queued < target_frames {
                output.fill(0);
                playback.last_value = 0.0;
                return;
            }
            playback.state = PlaybackState::Playing;
            playback.smoothed_error = 0.0;
            playback.fade_in_progress = 0;
        }

        let mut stale_position = playback.position;
        let mut crossfade_frames = 0usize;
        if queued > target_frames + RESYNC_HIGH_WATER_FRAMES {
            playback.position = write_frame as f64 - target_frames;
            queued = target_frames;
            playback.smoothed_error = 0.0;
            crossfade_frames = num_frames.min(RESYNC_FADE_FRAMES as usize);
            self.resyncs.fetch_add(1, Ordering::Relaxed);
        }
        playback.adjustment = update_controller(
            &mut playback.smoothed_error,
            &mut playback.integral,
            queued - target_frames,
            num_frames as f64 / SAMPLE_RATE_FRAMES,
        );

        let step = 1.0 + playback.adjustment;
        let mut frame = 0usize;
        while frame < num_frames && has_lookahead(playback.position, write_frame) {
            let mut value = self.interpolate(playback.position);
            if frame < crossfade_frames {
                let blend = (frame + 1) as f32 / crossfade_frames as f32;
                let stale = self.interpolate(stale_position);
                value = stale + (value - stale) * blend;
                stale_position += step;
            }
            if playback.fade_in_progress < START_FADE_FRAMES {
                value *= playback.fade_in_progress as f32 / START_FADE_FRAMES as f32;
                playback.fade_in_progress += 1;
            }
            playback.last_value = value;
            output[frame] = to_pcm16(value);
            playback.position += step;
            frame += 1;
        }
        if frame < num_frames {
            for out in &mut output[frame..] {
                playback.last_value *= STARVATION_DECAY;
                *out = to_pcm16(playback.last_value);
            }
            playback.state = PlaybackState::Priming;
            playback.extra_target_frames = (playback.extra_target_frames
                + UNDERRUN_TARGET_STEP_FRAMES)
                .min(MAXIMUM_EXTRA_TARGET_FRAMES);
            self.underruns.fetch_add(1, Ordering::Relaxed);
        }

        let integer = playback.position as u64;
        self.ring.publish_read_index(if integer > InterpolationFilter::HISTORY_FRAMES as u64 {
            integer - InterpolationFilter::HISTORY_FRAMES as u64
        } else {
            0
        });
        self.stat_fill_frames.store(queued as i32, Ordering::Relaxed);
        self.stat_target_frames.store(target_frames as i32, Ordering::Relaxed);
        self.stat_adjustment_ppm
            .store((playback.adjustment * 1.0e6) as i32, Ordering::Relaxed);
    }

    fn interpolate(&self, sample_position: f64) -> f32 {
        let base = sample_position as u64;
        self.filter
            .apply(&self.ring, base, sample_position - base as f64)
    }

    fn wait_for(&self, events: i16) -> bool {
        let mut descriptor = libc::pollfd {
            fd: self.descriptor.load(Ordering::Relaxed),
            events,
            revents: 0,
        };
        while self.running.load(Ordering::Acquire) {
            let result = unsafe { libc::poll(&mut descriptor, 1, TRANSPORT_POLL_TIMEOUT_MS) };
            if result > 0 {
                return (descriptor.revents & events) != 0
                    && (descriptor.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL))
                        == 0;
            }
            if result < 0 {
                let error = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if error != libc::EINTR {
                    return false;
                }
            }
        }
        false
    }

    fn read_available(
        &self,
        data: *mut u8,
        capacity: usize,
        received: &mut usize,
        ready_time: &mut Instant,
    ) -> bool {
        let descriptor = self.descriptor.load(Ordering::Relaxed);
        while self.running.load(Ordering::Acquire) {
            if !self.wait_for(libc::POLLIN) {
                return false;
            }
            *ready_time = Instant::now();
            let count = unsafe { libc::read(descriptor, data as *mut c_void, capacity) };
            if count > 0 {
                *received = count as usize;
                return true;
            }
            if count == 0 {
                return false;
            }
            let error = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if error != libc::EINTR && error != libc::EAGAIN && error != libc::EWOULDBLOCK {
                return false;
            }
        }
        false
    }

    fn write_ready(&self) -> bool {
        let ready: u8 = RECEIVER_READY;
        let descriptor = self.descriptor.load(Ordering::Relaxed);
        while self.running.load(Ordering::Acquire) {
            if !self.wait_for(libc::POLLOUT) {
                return false;
            }
            let count = unsafe {
                libc::write(
                    descriptor,
                    (&ready as *const u8) as *const c_void,
                    std::mem::size_of::<u8>(),
                )
            };
            if count == std::mem::size_of::<u8>() as isize {
                return true;
            }
            if count < 0 {
                let error = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
                if error != libc::EINTR && error != libc::EAGAIN && error != libc::EWOULDBLOCK {
                    return false;
                }
            }
        }
        false
    }

    fn run_transport(self: &Arc<Self>) {
        let thread_id = unsafe { libc::gettid() } as i32;
        unsafe {
            libc::setpriority(
                libc::PRIO_PROCESS,
                thread_id as libc::id_t,
                TRANSPORT_NICE_PRIORITY,
            );
        }
        let hint = PerformanceHint::open(thread_id, TRANSPORT_TARGET_WORK_NANOS);
        let mut samples = [0i16; READ_REQUEST_BYTES / std::mem::size_of::<i16>() + 1];
        let bytes = samples.as_mut_ptr() as *mut u8;
        let mut carry_bytes = 0usize;
        let mut ready_time = Instant::now();
        let mut worst_work_nanos = 0i64;
        let mut packets_since_report = 0u32;
        while self.running.load(Ordering::Acquire) {
            let mut received = 0usize;
            // SAFETY: carry_bytes <= 1 and capacity == 4096, within the
            // 4098-byte sample buffer.
            if !self.read_available(
                unsafe { bytes.add(carry_bytes) },
                READ_REQUEST_BYTES,
                &mut received,
                &mut ready_time,
            ) {
                if self.running.load(Ordering::Acquire) {
                    self.running.store(false, Ordering::Release);
                }
                break;
            }
            let total_bytes = carry_bytes + received;
            let whole_samples = total_bytes / std::mem::size_of::<i16>();
            if whole_samples > 0 && !self.ring.write(&samples[..whole_samples]) {
                self.dropped_chunks.fetch_add(1, Ordering::Relaxed);
            }
            carry_bytes = total_bytes - whole_samples * std::mem::size_of::<i16>();
            if carry_bytes > 0 {
                unsafe {
                    *bytes = *bytes.add(whole_samples * std::mem::size_of::<i16>());
                }
            }
            let Some(hint) = hint.as_ref() else { continue };
            let work_nanos = ready_time.elapsed().as_nanos() as i64;
            worst_work_nanos = worst_work_nanos.max(work_nanos);
            packets_since_report += 1;
            if packets_since_report >= TRANSPORT_REPORT_INTERVAL_PACKETS {
                hint.report(worst_work_nanos);
                worst_work_nanos = 0;
                packets_since_report = 0;
            }
        }
    }

    fn run_readiness(self: &Arc<Self>) {
        let mut ticks = 0u32;
        let mut logged_underruns = 0u32;
        let mut logged_resyncs = 0u32;
        let mut logged_drops = 0u32;
        while self.running.load(Ordering::Acquire) {
            thread::sleep(Duration::from_millis(READINESS_INTERVAL_MS));
            if !self.running.load(Ordering::Acquire) {
                return;
            }
            if !self.write_ready() {
                if self.running.load(Ordering::Acquire) {
                    self.running.store(false, Ordering::Release);
                }
                return;
            }
            ticks += 1;
            let current_underruns = self.underruns.load(Ordering::Relaxed);
            let current_resyncs = self.resyncs.load(Ordering::Relaxed);
            let current_drops = self.dropped_chunks.load(Ordering::Relaxed);
            let changed = current_underruns != logged_underruns
                || current_resyncs != logged_resyncs
                || current_drops != logged_drops;
            if changed || ticks % STATS_INTERVAL_TICKS == 0 {
                log_stats(
                    self.stat_fill_frames.load(Ordering::Relaxed),
                    self.stat_target_frames.load(Ordering::Relaxed),
                    self.stat_adjustment_ppm.load(Ordering::Relaxed),
                    current_underruns,
                    current_resyncs,
                    current_drops,
                );
                logged_underruns = current_underruns;
                logged_resyncs = current_resyncs;
                logged_drops = current_drops;
            }
        }
    }
}

impl Drop for Receiver {
    fn drop(&mut self) {
        self.stop();
    }
}

fn update_controller(
    smoothed_error: &mut f64,
    integral: &mut f64,
    error_frames: f64,
    delta_seconds: f64,
) -> f64 {
    let smoothing = delta_seconds / (ERROR_SMOOTHING_SECONDS + delta_seconds);
    *smoothed_error += (error_frames - *smoothed_error) * smoothing;
    *integral = (*integral + *smoothed_error * INTEGRAL_GAIN * delta_seconds)
        .clamp(-MAXIMUM_ADJUSTMENT, MAXIMUM_ADJUSTMENT);
    (*smoothed_error * PROPORTIONAL_GAIN + *integral).clamp(-MAXIMUM_ADJUSTMENT, MAXIMUM_ADJUSTMENT)
}

fn has_lookahead(sample_position: f64, write_frame: u64) -> bool {
    ((sample_position as u64) + InterpolationFilter::LOOKAHEAD_FRAMES as u64) < write_frame
}
