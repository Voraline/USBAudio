//! Hand-rolled JNI bridge, mirroring the JNI_OnLoad registration in
//! android/app/src/main/cpp/Receiver.cpp. Uses the stable JNI ABI function
//! table indices (JNI 1.6): FindClass = 6, RegisterNatives = 215,
//! JNIInvokeInterface::GetEnv = 6.

use std::ffi::{c_char, c_int, c_void};
use std::sync::{Arc, Mutex};

use super::receiver::Receiver;

#[allow(non_camel_case_types)]
pub type jint = i32;
#[allow(non_camel_case_types)]
pub type jboolean = u8;
#[allow(non_camel_case_types)]
pub type jobject = *mut c_void;

pub const JNI_FALSE: jboolean = 0;
pub const JNI_TRUE: jboolean = 1;
pub const JNI_VERSION_1_6: jint = 0x00010006;
pub const JNI_OK: c_int = 0;
pub const JNI_ERR: c_int = -1;

#[repr(C)]
struct JNINativeMethod {
    name: *const c_char,
    signature: *const c_char,
    fn_ptr: *mut c_void,
}

static ACTIVE_RECEIVER: Mutex<Option<Arc<Receiver>>> = Mutex::new(None);

unsafe extern "C" fn start_native(_env: *mut c_void, _object: jobject, descriptor: jint) -> jboolean {
    let mut guard = ACTIVE_RECEIVER.lock().unwrap();
    *guard = None;
    let receiver = Arc::new(Receiver::new(descriptor));
    if !receiver.start() {
        return JNI_FALSE;
    }
    *guard = Some(receiver);
    JNI_TRUE
}

unsafe extern "C" fn stop_native(_env: *mut c_void, _object: jobject) {
    let mut guard = ACTIVE_RECEIVER.lock().unwrap();
    *guard = None;
}

unsafe extern "C" fn is_running_native(_env: *mut c_void, _object: jobject) -> jboolean {
    let guard = ACTIVE_RECEIVER.lock().unwrap();
    if guard.as_ref().is_some_and(|receiver| receiver.is_running()) {
        JNI_TRUE
    } else {
        JNI_FALSE
    }
}

type EnvTable = *const usize;

unsafe fn table_of(env: *mut c_void) -> EnvTable {
    *(env as *const *const usize)
}

unsafe fn find_class(env: *mut c_void, name: *const c_char) -> jobject {
    let find_class: unsafe extern "C" fn(*mut c_void, *const c_char) -> jobject =
        std::mem::transmute(*table_of(env).add(6));
    find_class(env, name)
}

unsafe fn register_natives(
    env: *mut c_void,
    class: jobject,
    methods: *const JNINativeMethod,
    count: c_int,
) -> c_int {
    let register_natives: unsafe extern "C" fn(
        *mut c_void,
        jobject,
        *const JNINativeMethod,
        c_int,
    ) -> c_int = std::mem::transmute(*table_of(env).add(215));
    register_natives(env, class, methods, count)
}

/// `JNI_OnLoad` entry point, exported from libusb_audio_native.so.
#[no_mangle]
pub unsafe extern "C" fn JNI_OnLoad(vm: *mut c_void, _reserved: *mut c_void) -> jint {
    let get_env: unsafe extern "C" fn(*mut c_void, *mut c_int, *mut *mut c_void) -> c_int =
        std::mem::transmute(*table_of(vm).add(6));
    let mut environment: *mut c_void = std::ptr::null_mut();
    let mut version = JNI_VERSION_1_6;
    if get_env(vm, &mut version, &mut environment) != JNI_OK {
        return JNI_ERR;
    }
    let service_class =
        find_class(environment, b"com/example/usbaudio/AudioService\0".as_ptr() as *const c_char);
    if service_class.is_null() {
        return JNI_ERR;
    }
    let methods = [
        JNINativeMethod {
            name: b"StartNative\0".as_ptr() as *const c_char,
            signature: b"(I)Z\0".as_ptr() as *const c_char,
            fn_ptr: start_native as *mut c_void,
        },
        JNINativeMethod {
            name: b"StopNative\0".as_ptr() as *const c_char,
            signature: b"()V\0".as_ptr() as *const c_char,
            fn_ptr: stop_native as *mut c_void,
        },
        JNINativeMethod {
            name: b"IsRunningNative\0".as_ptr() as *const c_char,
            signature: b"()Z\0".as_ptr() as *const c_char,
            fn_ptr: is_running_native as *mut c_void,
        },
    ];
    if register_natives(
        environment,
        service_class,
        methods.as_ptr(),
        methods.len() as c_int,
    ) != JNI_OK
    {
        return JNI_ERR;
    }
    JNI_VERSION_1_6
}
