use crate::error::{ApiError, Result};
use serde::{Deserialize, Serialize};
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
    #[serde(skip)]
    pub browser: Option<BrowserEncoding>,
}
pub const H264_MAX_QP: u32 = 40;
pub const AV1_DEFAULT_MAX_MBPS: u32 = 18;
// AV1 level 4.1 Main tier limits 1080p60 video to 20 Mbps.
pub const AV1_MAX_MBPS: u32 = 20;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub struct Av1Vbr {
    pub target_bps: u32,
    pub max_bps: u32,
}
impl Av1Vbr {
    pub fn from_max_mbps(max_mbps: u32) -> Result<Self> {
        if !(1..=AV1_MAX_MBPS).contains(&max_mbps) {
            return Err(ApiError::invalid(format!(
                "AV1 peak bitrate must be between 1 and {AV1_MAX_MBPS} Mbps"
            )));
        }
        let max_bps = max_mbps * 1_000_000;
        Ok(Self {
            target_bps: max_bps * 2 / 3,
            max_bps,
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BrowserEncoding {
    H264 { qp: u32 },
    Av1(Av1Vbr),
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
#[derive(Clone, Copy, Default, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum BrowserCodec {
    #[default]
    H264,
    Av1,
}
impl BrowserCodec {
    pub const ALL: [Self; 2] = [Self::H264, Self::Av1];
}
impl Output {
    pub fn encoder(&self) -> &'static str {
        match self.browser {
            Some(BrowserEncoding::H264 { .. }) => Codec::H264.encoder(),
            Some(BrowserEncoding::Av1(_)) => "av1_nvenc",
            None => self.codec.encoder(),
        }
    }

    pub fn quantizer(&self) -> u32 {
        match self.browser {
            Some(BrowserEncoding::H264 { qp }) => qp,
            _ => self.qp,
        }
    }

    pub fn av1_vbr(&self) -> Option<Av1Vbr> {
        match self.browser {
            Some(BrowserEncoding::Av1(vbr)) => Some(vbr),
            _ => None,
        }
    }
}
impl Default for Output {
    fn default() -> Self {
        Self {
            segment_seconds: 2,
            codec: Codec::Hevc,
            qp: 18,
            browser: None,
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
            || self.output.qp > H264_MAX_QP
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
