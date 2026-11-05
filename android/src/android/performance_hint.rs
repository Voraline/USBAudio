//! Minimal FFI bindings for the Android performance hint API
//! (android/performance_hint.h), mirroring the C++ PerformanceHint class.

use std::ffi::c_void;
use std::ptr::null_mut;

#[link(name = "android")]
extern "C" {
    fn APerformanceHint_getManager() -> *mut c_void;
    fn APerformanceHint_createSession(
        manager: *mut c_void,
        thread_ids: *const i32,
        size: usize,
        target_duration_nanos: i64,
    ) -> *mut c_void;
    fn APerformanceHint_reportActualWorkDuration(session: *mut c_void, actual_duration_nanos: i64);
    fn APerformanceHint_closeSession(session: *mut c_void);
}

pub struct PerformanceHint {
    session: *mut c_void,
}

impl PerformanceHint {
    pub fn open(thread_id: i32, target_duration_nanos: i64) -> Option<Self> {
        let manager = unsafe { APerformanceHint_getManager() };
        if manager.is_null() {
            return None;
        }
        let session = unsafe {
            APerformanceHint_createSession(manager, &thread_id, 1, target_duration_nanos)
        };
        if session.is_null() {
            None
        } else {
            Some(Self { session })
        }
    }

    pub fn report(&self, actual_duration_nanos: i64) {
        if !self.session.is_null() {
            unsafe {
                APerformanceHint_reportActualWorkDuration(self.session, actual_duration_nanos);
            }
        }
    }

    fn close(&mut self) {
        if !self.session.is_null() {
            unsafe {
                APerformanceHint_closeSession(self.session);
            }
            self.session = null_mut();
        }
    }
}

impl Drop for PerformanceHint {
    fn drop(&mut self) {
        self.close();
    }
}
