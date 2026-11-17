use crate::ApiError;
use serde::Serialize;
use std::sync::Arc;

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, Serialize)]
pub struct Info {
    pub width: i32,
    pub height: i32,
    pub duration: f64,
    pub source_fps: f64,
}
#[derive(Debug)]
pub struct Segment {
    pub bytes: Vec<u8>,
    pub frames: i32,
    pub seconds: f64,
}

#[cfg(feature = "native")]
mod implementation {
    use super::*;
    use std::ffi::{c_char, c_int, c_void, CStr, CString};
    use std::ptr::NonNull;
    #[repr(C)]
    #[derive(Default)]
    struct RawSegment {
        data: *const u8,
        length: usize,
        frames: c_int,
        seconds: f64,
        owner: *mut c_void,
    }
    extern "C" {
        fn modeld_context_create(model: *const c_char, cache: *const c_char) -> *mut c_void;
        fn modeld_context_free(context: *mut c_void);
        fn modeld_video_open(
            context: *mut c_void,
            url: *const c_char,
            source_headers: *const c_char,
            seconds: c_int,
            qp: c_int,
            encoder: *const c_char,
            info: *mut Info,
        ) -> *mut c_void;
        fn modeld_video_cancel(video: *mut c_void);
        fn modeld_video_free(video: *mut c_void);
        fn modeld_video_render(video: *mut c_void, sequence: i64, out: *mut RawSegment) -> c_int;
        fn modeld_segment_free(out: *mut RawSegment);
        fn modeld_last_error() -> *const c_char;
        fn modeld_last_status() -> c_int;
    }
    fn error() -> ApiError {
        unsafe {
            ApiError::new(
                modeld_last_status() as u16,
                CStr::from_ptr(modeld_last_error())
                    .to_string_lossy()
                    .into_owned(),
            )
        }
    }
    fn cstring(s: &str) -> Result<CString, ApiError> {
        CString::new(s).map_err(|_| ApiError::new(422, "Embedded NUL in native argument"))
    }
    pub struct Backend {
        ptr: NonNull<c_void>,
        render_lock: std::sync::Mutex<()>,
    }
    // Backend profile mutation is protected by its C++ mutex. GPU work is dispatched
    // exclusively on the scheduler thread; each call sets the primary CUDA context.
    unsafe impl Send for Backend {}
    unsafe impl Sync for Backend {}
    impl Backend {
        pub fn new(model: &str, cache: &str) -> Result<Arc<Self>, ApiError> {
            let m = cstring(model)?;
            let c = cstring(cache)?;
            let ptr = NonNull::new(unsafe { modeld_context_create(m.as_ptr(), c.as_ptr()) })
                .ok_or_else(error)?;
            Ok(Arc::new(Self {
                ptr,
                render_lock: std::sync::Mutex::new(()),
            }))
        }
    }
    impl Drop for Backend {
        fn drop(&mut self) {
            unsafe { modeld_context_free(self.ptr.as_ptr()) }
        }
    }
    pub struct Video {
        ptr: NonNull<c_void>,
        pub info: Info,
        _backend: Arc<Backend>,
    }
    // Opening/probing precedes publication. Render is called only on the one GPU
    // scheduler thread. The sole concurrent native operation is atomic cancellation.
    unsafe impl Send for Video {}
    unsafe impl Sync for Video {}
    impl Video {
        pub fn open(
            backend: Arc<Backend>,
            url: &str,
            source_headers: &str,
            seconds: i32,
            qp: i32,
            encoder: &str,
        ) -> Result<Arc<Self>, ApiError> {
            let u = cstring(url)?;
            let headers = cstring(source_headers)?;
            let encoder = cstring(encoder)?;
            let mut info = Info::default();
            let ptr = NonNull::new(unsafe {
                modeld_video_open(
                    backend.ptr.as_ptr(),
                    u.as_ptr(),
                    headers.as_ptr(),
                    seconds,
                    qp,
                    encoder.as_ptr(),
                    &mut info,
                )
            })
            .ok_or_else(error)?;
            Ok(Arc::new(Self {
                ptr,
                info,
                _backend: backend,
            }))
        }
        pub fn cancel(&self) {
            unsafe { modeld_video_cancel(self.ptr.as_ptr()) }
        }
        pub fn render(&self, sequence: i64) -> Result<Segment, ApiError> {
            let _guard = self
                ._backend
                .render_lock
                .lock()
                .map_err(|_| ApiError::new(503, "GPU rendering lock was poisoned"))?;
            let mut out = RawSegment::default();
            if unsafe { modeld_video_render(self.ptr.as_ptr(), sequence, &mut out) } != 0 {
                return Err(error());
            }
            let bytes = unsafe { std::slice::from_raw_parts(out.data, out.length) }.to_vec();
            let result = Segment {
                bytes,
                frames: out.frames,
                seconds: out.seconds,
            };
            unsafe { modeld_segment_free(&mut out) };
            Ok(result)
        }
    }
    impl Drop for Video {
        fn drop(&mut self) {
            unsafe { modeld_video_free(self.ptr.as_ptr()) }
        }
    }
}
#[cfg(not(feature = "native"))]
mod implementation {
    use super::*;
    pub struct Backend;
    impl Backend {
        pub fn new(_: &str, _: &str) -> Result<Arc<Self>, ApiError> {
            Err(ApiError::new(503, "Native GPU backend was not compiled"))
        }
    }
    pub struct Video {
        pub info: Info,
    }
    impl Video {
        pub fn open(
            _: Arc<Backend>,
            _: &str,
            _: &str,
            _: i32,
            _: i32,
            _: &str,
        ) -> Result<Arc<Self>, ApiError> {
            Err(ApiError::new(503, "Native GPU backend was not compiled"))
        }
        pub fn cancel(&self) {}
        pub fn render(&self, _: i64) -> Result<Segment, ApiError> {
            Err(ApiError::new(503, "Native GPU backend was not compiled"))
        }
    }
}
pub use implementation::{Backend, Video};
