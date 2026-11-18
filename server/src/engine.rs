use crate::{config::Config, error::Result, native::Cancellation};
use modeld_protocol::{CreateSession, Media};
use std::sync::Arc;

#[derive(Debug)]
pub struct Segment {
    pub bytes: Vec<u8>,
    pub frames: u32,
    pub render_seconds: f64,
}
pub trait Stream: Send + Sync {
    fn content_type(&self) -> &str {
        "video/x-matroska"
    }
    fn media(&self) -> &Media;
    fn initialization(&self) -> Arc<Vec<u8>>;
    fn render(&self, index: u64) -> Result<Segment>;
    fn cancel(&self);
}
pub trait Engine: Send + Sync {
    fn open(
        self: Arc<Self>,
        request: &CreateSession,
        config: &Config,
        cancellation: Arc<Cancellation>,
    ) -> Result<Arc<dyn Stream>>;
}
