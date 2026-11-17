use crate::error::{ApiError, Result};
use serde::Deserialize;
use std::{net::SocketAddr, path::PathBuf};

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub listen: SocketAddr,
    pub token_file: PathBuf,
    pub inference: Inference,
    #[serde(default)]
    pub limits: Limits,
    #[serde(default)]
    pub output: Output,
}
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Inference {
    pub model_file: String,
    pub cache_directory: String,
}
#[derive(Clone, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Limits {
    pub sessions: usize,
    pub queued_segments: usize,
    pub cached_segments: usize,
    pub lease_seconds: u64,
    pub source_timeout_seconds: u32,
}
impl Default for Limits {
    fn default() -> Self {
        Self {
            sessions: 2,
            queued_segments: 8,
            cached_segments: 12,
            lease_seconds: 120,
            source_timeout_seconds: 30,
        }
    }
}
#[derive(Clone, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Output {
    pub segment_seconds: u32,
    pub codec: Codec,
    pub qp: u32,
}
#[derive(Clone, Copy, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Codec {
    Hevc,
    H264,
}
impl Codec {
    pub fn encoder(self) -> &'static str {
        match self {
            Self::Hevc => "hevc_nvenc",
            Self::H264 => "h264_nvenc",
        }
    }
}
impl Default for Output {
    fn default() -> Self {
        Self {
            segment_seconds: 2,
            codec: Codec::Hevc,
            qp: 18,
        }
    }
}
impl Config {
    pub fn validate(&self) -> Result<()> {
        let l = &self.limits;
        if !(1..=16).contains(&l.sessions)
            || !(1..=64).contains(&l.queued_segments)
            || !(1..=256).contains(&l.cached_segments)
            || !(30..=86400).contains(&l.lease_seconds)
            || !(1..=120).contains(&l.source_timeout_seconds)
            || !(1..=10).contains(&self.output.segment_seconds)
            || self.output.qp > 40
            || self.inference.model_file.is_empty()
            || self.inference.cache_directory.is_empty()
        {
            return Err(ApiError::new(
                500,
                "invalid_config",
                "Invalid server configuration",
            ));
        }
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn configuration_is_strict_and_bounded() {
        let value = serde_json::json!({"listen":"127.0.0.1:8765","token_file":"/private/token",
            "inference":{"model_file":"/model.onnx","cache_directory":"/cache"}});
        let mut c: Config = serde_json::from_value(value.clone()).unwrap();
        assert!(c.validate().is_ok());
        c.limits.queued_segments = 0;
        assert!(c.validate().is_err());
        let mut bad = value;
        bad["public_url"] = "http://old.example".into();
        assert!(serde_json::from_value::<Config>(bad).is_err());
    }
}
