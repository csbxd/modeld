use crate::{
    config::Config,
    engine::{Engine, Segment, Stream},
    error::{ApiError, Result},
};
use modeld_protocol::{CreateSession, Media};
use std::{
    ffi::{c_char, c_int, c_void, CStr, CString},
    ptr::NonNull,
    sync::Arc,
};

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
    fn modeld_cancel_token_create() -> *mut c_void;
    fn modeld_cancel_token_set(token: *mut c_void);
    #[cfg(test)]
    fn modeld_cancel_token_is_set(token: *mut c_void) -> c_int;
    fn modeld_cancel_token_free(token: *mut c_void);
    fn modeld_stream_open(
        context: *mut c_void,
        request: *const c_char,
        settings: *const c_char,
        cancellation: *mut c_void,
    ) -> *mut c_void;
    fn modeld_stream_description(stream: *mut c_void) -> *const c_char;
    fn modeld_stream_content_type(stream: *mut c_void) -> *const c_char;
    fn modeld_stream_init(stream: *mut c_void, out: *mut RawSegment) -> c_int;
    fn modeld_stream_render(stream: *mut c_void, index: u64, out: *mut RawSegment) -> c_int;
    fn modeld_stream_cancel(stream: *mut c_void);
    fn modeld_stream_free(stream: *mut c_void);
    fn modeld_segment_free(out: *mut RawSegment);
    fn modeld_last_error() -> *const c_char;
    fn modeld_last_status() -> c_int;
}
impl Drop for RawSegment {
    fn drop(&mut self) {
        if !self.owner.is_null() {
            unsafe { modeld_segment_free(self) }
        }
    }
}
impl RawSegment {
    fn bytes(&self) -> Vec<u8> {
        if self.length == 0 {
            Vec::new()
        } else {
            unsafe { std::slice::from_raw_parts(self.data, self.length) }.to_vec()
        }
    }
}
fn error() -> ApiError {
    unsafe {
        ApiError::native(
            modeld_last_status() as u16,
            CStr::from_ptr(modeld_last_error())
                .to_string_lossy()
                .into_owned(),
        )
    }
}
fn cstring(value: &str) -> Result<CString> {
    CString::new(value).map_err(|_| ApiError::invalid("Native input contains NUL"))
}
pub struct Cancellation(NonNull<c_void>);
// The token exclusively owns a shared atomic flag. It never accesses FFmpeg objects.
unsafe impl Send for Cancellation {}
unsafe impl Sync for Cancellation {}
impl Cancellation {
    pub fn new() -> Result<Arc<Self>> {
        NonNull::new(unsafe { modeld_cancel_token_create() })
            .map(|p| Arc::new(Self(p)))
            .ok_or_else(error)
    }
    pub fn cancel(&self) {
        unsafe { modeld_cancel_token_set(self.0.as_ptr()) }
    }
    #[cfg(test)]
    pub fn canceled(&self) -> bool {
        unsafe { modeld_cancel_token_is_set(self.0.as_ptr()) != 0 }
    }
}
impl Drop for Cancellation {
    fn drop(&mut self) {
        unsafe { modeld_cancel_token_free(self.0.as_ptr()) }
    }
}
pub struct Backend {
    ptr: NonNull<c_void>,
}
// C++ protects its profile cache. GPU rendering is exclusive to Scheduler's worker.
unsafe impl Send for Backend {}
unsafe impl Sync for Backend {}
impl Backend {
    pub fn new(model: &str, cache: &str) -> Result<Arc<Self>> {
        let model = cstring(model)?;
        let cache = cstring(cache)?;
        NonNull::new(unsafe { modeld_context_create(model.as_ptr(), cache.as_ptr()) })
            .map(|ptr| Arc::new(Self { ptr }))
            .ok_or_else(error)
    }
}
impl Drop for Backend {
    fn drop(&mut self) {
        unsafe { modeld_context_free(self.ptr.as_ptr()) }
    }
}
struct NativeStream {
    ptr: NonNull<c_void>,
    media: Media,
    initialization: Arc<Vec<u8>>,
    content_type: String,
    _backend: Arc<Backend>,
}
// Stream is published after probing. Only Scheduler calls render; cancel only sets an atomic flag.
unsafe impl Send for NativeStream {}
unsafe impl Sync for NativeStream {}
struct Guard(NonNull<c_void>);
impl Drop for Guard {
    fn drop(&mut self) {
        unsafe { modeld_stream_free(self.0.as_ptr()) }
    }
}
impl Engine for Backend {
    fn open(
        self: Arc<Self>,
        request: &CreateSession,
        config: &Config,
        cancellation: Arc<Cancellation>,
    ) -> Result<Arc<dyn Stream>> {
        let request = cstring(
            &serde_json::to_string(request)
                .map_err(|_| ApiError::invalid("Invalid native input"))?,
        )?;
        let settings = cstring(
            &serde_json::json!({"segment_seconds":config.output.segment_seconds,
            "encoder":config.output.encoder(),"qp":config.output.qp,
            "source_timeout_seconds":config.limits.source_timeout_seconds,"fragmented_mp4":config.output.browser.is_some()})
            .to_string(),
        )?;
        let guard = Guard(
            NonNull::new(unsafe {
                modeld_stream_open(
                    self.ptr.as_ptr(),
                    request.as_ptr(),
                    settings.as_ptr(),
                    cancellation.0.as_ptr(),
                )
            })
            .ok_or_else(error)?,
        );
        let description = unsafe { CStr::from_ptr(modeld_stream_description(guard.0.as_ptr())) };
        let media: Media = serde_json::from_slice(description.to_bytes()).map_err(|_| {
            ApiError::new(
                502,
                "invalid_backend_output",
                "Invalid native media description",
            )
        })?;
        let mut header = RawSegment::default();
        if unsafe { modeld_stream_init(guard.0.as_ptr(), &mut header) } != 0 {
            return Err(error());
        }
        let initialization = Arc::new(header.bytes());
        let content_type = unsafe { CStr::from_ptr(modeld_stream_content_type(guard.0.as_ptr())) }
            .to_string_lossy()
            .into_owned();
        if initialization.is_empty() {
            return Err(ApiError::new(
                502,
                "invalid_backend_output",
                "Empty Matroska initialization",
            ));
        }
        let ptr = guard.0;
        std::mem::forget(guard);
        Ok(Arc::new(NativeStream {
            ptr,
            media,
            initialization,
            content_type,
            _backend: self,
        }))
    }
}
impl Stream for NativeStream {
    fn content_type(&self) -> &str {
        &self.content_type
    }
    fn media(&self) -> &Media {
        &self.media
    }
    fn initialization(&self) -> Arc<Vec<u8>> {
        self.initialization.clone()
    }
    fn cancel(&self) {
        unsafe { modeld_stream_cancel(self.ptr.as_ptr()) }
    }
    fn render(&self, index: u64) -> Result<Segment> {
        let mut raw = RawSegment::default();
        if unsafe { modeld_stream_render(self.ptr.as_ptr(), index, &mut raw) } != 0 {
            return Err(error());
        }
        Ok(Segment {
            bytes: raw.bytes(),
            frames: raw.frames as u32,
            render_seconds: raw.seconds,
        })
    }
}
impl Drop for NativeStream {
    fn drop(&mut self) {
        unsafe { modeld_stream_free(self.ptr.as_ptr()) }
    }
}
