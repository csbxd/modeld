use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashSet};

pub const API: &str = "/api/v2";
pub const VERSION: u32 = 2;
pub const MODEL: &str = "rife-4.26-heavy-fp32";
pub const FPS: u32 = 60;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Input {
    pub urls: Vec<String>,
    #[serde(default)]
    pub headers: BTreeMap<String, String>,
}
impl Input {
    pub fn validate(&self) -> Result<(), String> {
        if self.urls.is_empty() || self.urls.len() > 9 {
            return Err("An input requires between one and nine URLs".into());
        }
        for value in &self.urls {
            let url = url::Url::parse(value).map_err(|_| "Invalid media URL")?;
            if !matches!(url.scheme(), "http" | "https")
                || url.host_str().is_none()
                || !url.username().is_empty()
                || url.password().is_some()
                || value.len() > 16384
                || value.chars().any(char::is_control)
            {
                return Err("Media URLs must use HTTP(S) without embedded credentials".into());
            }
        }
        let mut names = HashSet::new();
        let mut bytes = 0;
        if self.headers.len() > 16 {
            return Err("Too many input headers".into());
        }
        for (name, value) in &self.headers {
            let normalized = name.to_ascii_lowercase();
            if !matches!(
                normalized.as_str(),
                "referer"
                    | "user-agent"
                    | "cookie"
                    | "origin"
                    | "authorization"
                    | "accept"
                    | "accept-language"
                    | "x-forwarded-for"
            ) || !names.insert(normalized)
                || http::HeaderName::from_bytes(name.as_bytes()).is_err()
                || http::HeaderValue::from_str(value).is_err()
                || value.len() > 8192
            {
                return Err("Invalid or unsupported input header".into());
            }
            bytes += name.len() + value.len() + 4;
        }
        if bytes > 16384 {
            return Err("Input headers exceed 16 KiB".into());
        }
        Ok(())
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Tracks {
    pub audio: bool,
    pub subtitles: bool,
}
impl Default for Tracks {
    fn default() -> Self {
        Self {
            audio: true,
            subtitles: true,
        }
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct CreateSession {
    pub model: String,
    pub video: Input,
    #[serde(default)]
    pub audio: Option<Input>,
    #[serde(default)]
    pub tracks: Tracks,
}
impl CreateSession {
    pub fn validate(&self) -> Result<(), String> {
        if self.model != MODEL {
            return Err("Unknown model".into());
        }
        self.video.validate()?;
        if let Some(audio) = &self.audio {
            if !self.tracks.audio {
                return Err("Separate audio requires tracks.audio=true".into());
            }
            audio.validate()?;
        }
        Ok(())
    }
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum TrackKind {
    Video,
    Audio,
    Subtitle,
    Attachment,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum TrackSource {
    Video,
    Audio,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Track {
    pub id: u32,
    pub kind: TrackKind,
    pub source: TrackSource,
    pub source_index: u32,
    pub codec: String,
    pub language: String,
    pub title: String,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Media {
    pub width: u32,
    pub height: u32,
    pub duration_seconds: f64,
    pub source_fps: f64,
    pub output_fps: u32,
    pub segment_seconds: u32,
    pub segment_count: u64,
    pub tracks: Vec<Track>,
}
impl Media {
    pub fn segment_frames(&self, index: u64) -> Option<u32> {
        let begin = index.checked_mul(self.segment_seconds as u64 * FPS as u64)?;
        let frames = (self.duration_seconds * FPS as f64).ceil() as u64;
        if index >= self.segment_count || begin >= frames {
            return None;
        }
        Some((frames - begin).min(self.segment_seconds as u64 * FPS as u64) as u32)
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub schema: u32,
    pub media: Media,
    pub init: String,
    pub segment_template: String,
}
impl Manifest {
    pub fn new(id: &str, media: Media) -> Self {
        let base = format!("{API}/sessions/{id}/media");
        Self {
            schema: VERSION,
            media,
            init: format!("{base}/init"),
            segment_template: format!("{base}/segments/{{index}}"),
        }
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SessionCreated {
    pub id: String,
    pub lease_seconds: u64,
    pub manifest: Manifest,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Lease {
    pub paused: bool,
}
pub fn models() -> serde_json::Value {
    serde_json::json!({"models":[{
        "id":MODEL, "task":"frame_interpolation", "precision":"fp32",
        "tf32":false, "backend":"tensorrt-rtx", "output_fps":FPS,
        "input":{"max_long_side":1920,"max_short_side":1080,"dimension_multiple":2,
            "min_dimension":32,"max_fps_exclusive":60,"dynamic_range":"sdr","seekable":true},
        "output":{"container":"matroska","audio":"copy","subtitles":"copy"}
    }]})
}

#[cfg(test)]
mod tests {
    use super::*;
    fn request() -> serde_json::Value {
        serde_json::json!({"model":MODEL,"video":{"urls":["https://cdn.example/video"]}})
    }
    #[test]
    fn explicit_contract_rejects_unknown_fields_at_every_level() {
        let mut value = request();
        assert!(serde_json::from_value::<CreateSession>(value.clone())
            .unwrap()
            .validate()
            .is_ok());
        value["source_url"] = "https://old.example/video".into();
        assert!(serde_json::from_value::<CreateSession>(value).is_err());
        let mut value = request();
        value["video"]["backup_urls"] = serde_json::json!([]);
        assert!(serde_json::from_value::<CreateSession>(value).is_err());
        let mut value = request();
        value["tracks"] = serde_json::json!({"audio":true,"subtitles":true,"mode":"video"});
        assert!(serde_json::from_value::<CreateSession>(value).is_err());
        assert!(serde_json::from_value::<Lease>(serde_json::json!({})).is_err());
    }
    #[test]
    fn sources_and_headers_cannot_inject_transport_options() {
        let mut body: CreateSession = serde_json::from_value(request()).unwrap();
        for url in [
            "file:///etc/passwd",
            "http://user:password@host/video",
            "ftp://host/video",
            "https://cdn.example/invalid\0path",
        ] {
            body.video.urls = vec![url.into()];
            assert!(body.validate().is_err());
        }
        body.video.urls = vec!["http://100.123.1.2:1234/private".into()];
        body.video
            .headers
            .insert("Referer".into(), "https://www.bilibili.com/".into());
        assert!(body.validate().is_ok());
        body.video.headers.insert("Host".into(), "private".into());
        assert!(body.validate().is_err());
        body.video.headers.remove("Host");
        body.video
            .headers
            .insert("Cookie".into(), "x=y\r\nHost: other".into());
        assert!(body.validate().is_err());
    }
    #[test]
    fn timeline_preserves_fractional_last_frame_and_absolute_paths() {
        let media = Media {
            width: 320,
            height: 180,
            duration_seconds: 4.01,
            source_fps: 30.,
            output_fps: 60,
            segment_seconds: 2,
            segment_count: 3,
            tracks: vec![],
        };
        assert_eq!(media.segment_frames(2), Some(1));
        assert_eq!(media.segment_frames(3), None);
        assert_eq!(media.segment_frames(u64::MAX), None);
        let manifest = Manifest::new("opaque", media);
        assert_eq!(manifest.init, "/api/v2/sessions/opaque/media/init");
        assert_eq!(
            manifest.segment_template,
            "/api/v2/sessions/opaque/media/segments/{index}"
        );
    }
}
