//! Send/Sync wrappers for Win32 handle and pointer types, which windows-rs
//! models as raw pointers (and are therefore !Send/!Sync). Kernel handle
//! values are plain integers safe to share across threads; the C++ version
//! shares the same handles between its sender and main threads.

use windows::Win32::Foundation::HANDLE;
use windows::Win32::System::IO::OVERLAPPED;

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct SendHandle(pub HANDLE);

unsafe impl Send for SendHandle {}
unsafe impl Sync for SendHandle {}

#[derive(Clone, Copy)]
pub struct SendPtr<T>(pub *mut T);

unsafe impl<T> Send for SendPtr<T> {}
unsafe impl<T> Sync for SendPtr<T> {}

/// OVERLAPPED is !Send only because its hEvent field is a raw pointer; the
/// event is one of the link's own events, so sharing the whole struct across
/// the sender and main threads matches the C++ usage.
#[derive(Clone, Copy)]
pub struct SendOverlapped(pub OVERLAPPED);

unsafe impl Send for SendOverlapped {}
unsafe impl Sync for SendOverlapped {}
