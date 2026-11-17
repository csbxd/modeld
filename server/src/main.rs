mod native;
use axum::{
    body::Body,
    extract::{DefaultBodyLimit, Path, State},
    http::{HeaderMap, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use native::{Backend, Cancellation, Segment, Video};
use serde::Deserialize;
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, HashMap},
    fmt,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};
use subtle::ConstantTimeEq;
use tokio::sync::{mpsc, watch, OwnedSemaphorePermit, Semaphore};

const MODEL: &str = "rife-4.26-heavy-fp32";
#[derive(Debug, Clone)]
pub struct ApiError {
    status: u16,
    message: String,
}
impl ApiError {
    fn new(status: u16, message: impl Into<String>) -> Self {
        Self {
            status,
            message: message.into(),
        }
    }
}
impl fmt::Display for ApiError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}
impl std::error::Error for ApiError {}
impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (
            StatusCode::from_u16(self.status).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR),
            Json(json!({"error":self.message})),
        )
            .into_response()
    }
}
type Result<T> = std::result::Result<T, ApiError>;
type SegmentResult = Option<Result<Arc<Segment>>>;

#[derive(Clone, Deserialize)]
#[serde(default)]
struct Config {
    listen: String,
    port: u16,
    public_url: String,
    token_file: PathBuf,
    model_path: String,
    cache_dir: String,
    max_sessions: usize,
    queue_capacity: usize,
    segment_seconds: i32,
    cache_segments: usize,
    session_ttl_seconds: u64,
    source_timeout_seconds: i32,
    encoder_qp: i32,
    encoder: String,
}
impl Default for Config {
    fn default() -> Self {
        Self {
            listen: "127.0.0.1".into(),
            port: 8765,
            public_url: "http://s8030.netbird.cloud:8765".into(),
            token_file: "/etc/modeld/api.token".into(),
            model_path: "/usr/share/modeld/models/rife-4.26-heavy.onnx".into(),
            cache_dir: "/var/cache/modeld".into(),
            max_sessions: 2,
            queue_capacity: 8,
            segment_seconds: 2,
            cache_segments: 12,
            session_ttl_seconds: 120,
            source_timeout_seconds: 30,
            encoder_qp: 18,
            encoder: "hevc_nvenc".into(),
        }
    }
}
impl Config {
    fn validate(&self) -> Result<()> {
        if !(1..=16).contains(&self.max_sessions)
            || !(1..=64).contains(&self.queue_capacity)
            || !(1..=10).contains(&self.segment_seconds)
            || !(1..=256).contains(&self.cache_segments)
            || self.session_ttl_seconds < 30
            || !(1..=120).contains(&self.source_timeout_seconds)
            || !(0..=40).contains(&self.encoder_qp)
            || !matches!(self.encoder.as_str(), "h264_nvenc" | "hevc_nvenc")
        {
            return Err(ApiError::new(500, "Invalid server resource limits"));
        }
        source_url(&self.public_url)?;
        Ok(())
    }
}
fn source_url(url: &str) -> Result<()> {
    let parsed = url::Url::parse(url).map_err(|_| ApiError::new(422, "Invalid source URL"))?;
    if !matches!(parsed.scheme(), "http" | "https")
        || parsed.host_str().is_none()
        || !parsed.username().is_empty()
        || parsed.password().is_some()
    {
        return Err(ApiError::new(
            422,
            "A HTTP(S) source URL without embedded credentials is required",
        ));
    }
    Ok(())
}
fn random_id() -> Result<String> {
    let mut bytes = [0u8; 16];
    getrandom::fill(&mut bytes)
        .map_err(|_| ApiError::new(500, "Secure random generation failed"))?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}

struct Session {
    video: Arc<Video>,
    closed: Arc<AtomicBool>,
    last_used: Mutex<Instant>,
    paused: watch::Sender<bool>,
    cache: Mutex<BTreeMap<i64, watch::Receiver<SegmentResult>>>,
    _permit: OwnedSemaphorePermit,
}
impl Session {
    fn cancel(&self) {
        self.closed.store(true, Ordering::Release);
        self.video.cancel();
        self.paused.send_replace(false);
    }
    fn touch(&self) {
        *self.last_used.lock().unwrap() = Instant::now();
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.cancel();
    }
}
struct Job {
    video: Arc<Video>,
    closed: Arc<AtomicBool>,
    sequence: i64,
    result: watch::Sender<SegmentResult>,
}
struct App {
    config: Config,
    token: String,
    backend: Arc<Backend>,
    sessions: Mutex<HashMap<String, Arc<Session>>>,
    permits: Arc<Semaphore>,
    jobs: mpsc::Sender<Job>,
}
impl App {
    fn authorize(&self, headers: &HeaderMap) -> Result<()> {
        let expected = format!("Bearer {}", self.token);
        let given = headers
            .get("authorization")
            .map(|h| h.as_bytes())
            .unwrap_or_default();
        if !bool::from(given.ct_eq(expected.as_bytes())) {
            return Err(ApiError::new(401, "Authentication required"));
        }
        Ok(())
    }
    fn session(&self, id: &str) -> Result<Arc<Session>> {
        let sessions = self.sessions.lock().unwrap();
        let session = sessions
            .get(id)
            .filter(|s| !s.closed.load(Ordering::Acquire))
            .cloned()
            .ok_or_else(|| ApiError::new(404, "Session does not exist"))?;
        session.touch();
        Ok(session)
    }
    fn reap(&self) {
        self.sessions.lock().unwrap().retain(|_, s| {
            let keep = s.last_used.lock().unwrap().elapsed()
                < Duration::from_secs(self.config.session_ttl_seconds);
            if !keep {
                s.cancel()
            }
            keep
        });
    }
    fn cancel_all(&self) {
        for (_, s) in self.sessions.lock().unwrap().drain() {
            s.cancel()
        }
    }
    fn segment(&self, s: &Arc<Session>, sequence: i64) -> Result<watch::Receiver<SegmentResult>> {
        let total = (s.video.info.duration * 60.0).ceil() as i64;
        let begin = sequence
            .checked_mul(60 * self.config.segment_seconds as i64)
            .ok_or_else(|| ApiError::new(404, "Segment does not exist"))?;
        if sequence < 0 || begin >= total {
            return Err(ApiError::new(404, "Segment does not exist"));
        }
        if s.closed.load(Ordering::Acquire) {
            return Err(ApiError::new(410, "Session closed"));
        }
        let mut cache = s.cache.lock().unwrap();
        if let Some(entry) = cache.get(&sequence) {
            if !matches!(&*entry.borrow(), Some(Err(_))) {
                return Ok(entry.clone());
            }
            // A transient source error must not poison this segment for the
            // lifetime of the session. Existing waiters still see that error.
            cache.remove(&sequence);
        }
        let (tx, rx) = watch::channel(None);
        self.jobs
            .try_send(Job {
                video: s.video.clone(),
                closed: s.closed.clone(),
                sequence,
                result: tx,
            })
            .map_err(|_| ApiError::new(429, "GPU queue is full; retry later"))?;
        cache.insert(sequence, rx.clone());
        while cache.len() > self.config.cache_segments {
            let first = *cache.first_key_value().unwrap().0;
            cache.remove(&first);
        }
        Ok(rx)
    }
}

fn catalog() -> Value {
    json!({"object":"list","data":[{"id":MODEL,"task":"frame_interpolation","version":"4.26","variant":"heavy","precision":"fp32","tf32":false,"backend":"tensorrt-rtx","state":"ready","max_width":1920,"max_height":1920,"max_long_side":1920,"max_short_side":1080,"max_pixels":2073600,"target_fps":60,"output_modes":["video","main"]}]})
}
#[cfg(test)]
fn playlist(duration: f64, seconds: i32) -> Result<String> {
    media_playlist(duration, seconds, false)
}
fn media_playlist(duration: f64, seconds: i32, main: bool) -> Result<String> {
    if !duration.is_finite()
        || duration <= 0.0
        || duration > 604800.0
        || !(1..=10).contains(&seconds)
    {
        return Err(ApiError::new(422, "Invalid interpolation timeline"));
    }
    let frames = (duration * 60.0).ceil() as i64;
    let size = 60 * seconds as i64;
    let mut out=format!("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:{seconds}\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-INDEPENDENT-SEGMENTS\n");
    if main {
        out = out.replace("VERSION:3", "VERSION:6");
        out += "#EXT-X-MAP:URI=\"init.mkv\"\n";
    }
    for (index, begin) in (0..frames).step_by(size as usize).enumerate() {
        out += &format!(
            "#EXTINF:{:.6},\nsegments/{index}.{}\n",
            (frames - begin).min(size) as f64 / 60.0,
            if main { "mkv" } else { "ts" }
        )
    }
    out += "#EXT-X-ENDLIST\n";
    Ok(out)
}

async fn health(State(app): State<Arc<App>>) -> Result<Json<Value>> {
    if app.jobs.is_closed() {
        return Err(ApiError::new(503, "GPU scheduler is unavailable"));
    }
    Ok(Json(
        json!({"status":"ready","version":env!("CARGO_PKG_VERSION"),"model":MODEL}),
    ))
}
async fn models(State(app): State<Arc<App>>, headers: HeaderMap) -> Result<Json<Value>> {
    app.authorize(&headers)?;
    Ok(Json(catalog()))
}
#[derive(Default, Deserialize)]
struct CreateSession {
    source_url: String,
    #[serde(default)]
    source_backups: Vec<String>,
    #[serde(default)]
    source_headers: BTreeMap<String, String>,
    #[serde(default)]
    model: Option<String>,
    #[serde(default)]
    task: Option<String>,
    #[serde(default)]
    target_fps: Option<i32>,
    #[serde(default)]
    output_mode: Option<String>,
    #[serde(default)]
    audio_source_url: Option<String>,
    #[serde(default)]
    include_audio: Option<bool>,
    #[serde(default)]
    include_subtitles: Option<bool>,
}
impl CreateSession {
    fn http_headers(&self) -> Result<String> {
        if self.source_headers.len() > 16 {
            return Err(ApiError::new(422, "Too many source HTTP headers"));
        }
        let mut result = String::new();
        let mut seen = std::collections::HashSet::new();
        for (name, value) in &self.source_headers {
            let normalized = name.to_ascii_lowercase();
            if !matches!(
                normalized.as_str(),
                "user-agent"
                    | "referer"
                    | "origin"
                    | "cookie"
                    | "authorization"
                    | "accept"
                    | "accept-language"
                    | "x-forwarded-for"
            ) || !seen.insert(normalized)
                || axum::http::HeaderName::from_bytes(name.as_bytes()).is_err()
                || axum::http::HeaderValue::from_str(value).is_err()
                || value.len() > 8192
            {
                return Err(ApiError::new(
                    422,
                    "Invalid or unsupported source HTTP header",
                ));
            }
            result.push_str(name);
            result.push_str(": ");
            result.push_str(value);
            result.push_str("\r\n");
        }
        if result.len() > 16384 {
            return Err(ApiError::new(422, "Source HTTP headers are too large"));
        }
        Ok(result)
    }
    fn validate(&self) -> Result<()> {
        if !matches!(
            self.output_mode.as_deref().unwrap_or("video"),
            "video" | "main"
        ) {
            return Err(ApiError::new(422, "Unsupported output mode"));
        }
        if let Some(url) = self
            .audio_source_url
            .as_deref()
            .filter(|url| !url.is_empty())
        {
            source_url(url)?;
        }
        if self.model.as_deref().unwrap_or(MODEL) != MODEL {
            return Err(ApiError::new(404, "Unknown model"));
        }
        if self.task.as_deref().unwrap_or("frame_interpolation") != "frame_interpolation" {
            return Err(ApiError::new(
                501,
                "Only frame_interpolation is implemented in this release",
            ));
        }
        if self.target_fps.unwrap_or(60) != 60 {
            return Err(ApiError::new(422, "This release targets 60 fps"));
        }
        source_url(&self.source_url)?;
        if self.source_backups.len() > 8 {
            return Err(ApiError::new(422, "Too many source backup URLs"));
        }
        for backup in &self.source_backups {
            source_url(backup)?;
        }
        self.http_headers()?;
        Ok(())
    }
}
struct ProbeGuard(Option<Arc<Cancellation>>);
impl Drop for ProbeGuard {
    fn drop(&mut self) {
        if let Some(control) = &self.0 {
            control.cancel();
            eprintln!("[modeld] Source probe canceled before completion");
        }
    }
}
async fn create(
    State(app): State<Arc<App>>,
    headers: HeaderMap,
    Json(body): Json<CreateSession>,
) -> Result<(StatusCode, Json<Value>)> {
    app.authorize(&headers)?;
    body.validate()?;
    app.reap();
    let permit = app
        .permits
        .clone()
        .try_acquire_owned()
        .map_err(|_| ApiError::new(429, "Maximum active sessions reached"))?;
    let backend = app.backend.clone();
    let seconds = app.config.segment_seconds;
    let qp = app.config.encoder_qp;
    let encoder = app.config.encoder.clone();
    let source_timeout = app.config.source_timeout_seconds;
    let cancellation = Cancellation::new()?;
    let mut probe_guard = ProbeGuard(Some(cancellation.clone()));
    let source_headers = body.http_headers()?;
    let media_options = json!({"output_mode": body.output_mode.as_deref().unwrap_or("video"),
        "audio_source_url": body.audio_source_url.as_deref().unwrap_or(""),
        "include_audio": body.include_audio.unwrap_or(true),
        "include_subtitles": body.include_subtitles.unwrap_or(true)})
    .to_string();
    let mut sources = vec![body.source_url];
    sources.extend(body.source_backups);
    let sources =
        serde_json::to_string(&sources).map_err(|_| ApiError::new(422, "Invalid source URLs"))?;
    let started = Instant::now();
    eprintln!("[modeld] Source probe started; total limit {source_timeout}s");
    let probe = tokio::task::spawn_blocking(move || {
        // Keep the slot occupied until native probing actually exits, including
        // after an HTTP disconnect or timeout; detached tasks cannot bypass the limit.
        Video::open(
            backend,
            &sources,
            &source_headers,
            seconds,
            qp,
            &encoder,
            &native::SourceControl {
                timeout_seconds: source_timeout,
                cancellation,
                media_options,
            },
        )
        .map(|video| (video, permit))
    });
    let result =
        match tokio::time::timeout(Duration::from_secs(source_timeout as u64 + 5), probe).await {
            Ok(Ok(result)) => {
                probe_guard.0 = None;
                result
            }
            Ok(Err(_)) => {
                probe_guard.0 = None;
                Err(ApiError::new(500, "Source probe task failed"))
            }
            Err(_) => Err(ApiError::new(
                504,
                format!(
            "Source probe exceeded {source_timeout}s; check the source server or try a local file"
        ),
            )),
        };
    let (video, permit) = result.inspect_err(|error| {
        eprintln!(
            "[modeld] Source probe failed after {:.3}s: HTTP {}: {}",
            started.elapsed().as_secs_f64(),
            error.status,
            error
        );
    })?;
    eprintln!(
        "[modeld] Source probe ready after {:.3}s: {}x{} at {:.6} fps",
        started.elapsed().as_secs_f64(),
        video.info.width,
        video.info.height,
        video.info.source_fps
    );
    let id = random_id()?;
    let info = video.info;
    let media_metadata = video.metadata.clone();
    let (paused, _) = watch::channel(false);
    let session = Arc::new(Session {
        video,
        closed: Arc::new(AtomicBool::new(false)),
        last_used: Mutex::new(Instant::now()),
        paused,
        cache: Mutex::new(BTreeMap::new()),
        _permit: permit,
    });
    app.sessions.lock().unwrap().insert(id.clone(), session);
    probe_guard.0 = None;
    Ok((
        StatusCode::CREATED,
        Json(
            json!({"id":id,"model":MODEL,"task":"frame_interpolation","playlist_url":format!("{}/v1/sessions/{id}/index.m3u8",app.config.public_url.trim_end_matches('/')),"width":info.width,"height":info.height,"source_fps":info.source_fps,"duration":info.duration,"target_fps":60,"encoder":app.config.encoder,"encoder_qp":qp,"media":media_metadata}),
        ),
    ))
}
async fn remove(
    State(app): State<Arc<App>>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> Result<Json<Value>> {
    app.authorize(&headers)?;
    if let Some(s) = app.sessions.lock().unwrap().remove(&id) {
        s.cancel()
    }
    Ok(Json(json!({"status":"closed"})))
}
#[derive(Default, Deserialize)]
struct Heartbeat {
    #[serde(default)]
    paused: bool,
}
async fn heartbeat(
    State(app): State<Arc<App>>,
    headers: HeaderMap,
    Path(id): Path<String>,
    Json(body): Json<Heartbeat>,
) -> Result<Json<Value>> {
    app.authorize(&headers)?;
    let s = app.session(&id)?;
    s.paused.send_replace(body.paused);
    Ok(Json(json!({"status":"active"})))
}
async fn index(State(app): State<Arc<App>>, Path(id): Path<String>) -> Result<Response> {
    let s = app.session(&id)?;
    Ok((
        [
            ("content-type", "application/vnd.apple.mpegurl"),
            ("cache-control", "private, no-store"),
        ],
        media_playlist(
            s.video.info.duration,
            app.config.segment_seconds,
            !s.video.header.is_empty(),
        )?,
    )
        .into_response())
}
async fn init_segment(State(app): State<Arc<App>>, Path(id): Path<String>) -> Result<Response> {
    let s = app.session(&id)?;
    if s.video.header.is_empty() {
        return Err(ApiError::new(404, "No initialization segment"));
    }
    Ok(Response::builder()
        .header("content-type", "video/x-matroska")
        .header("cache-control", "private, max-age=3600")
        .body(Body::from(s.video.header.clone()))
        .unwrap())
}
async fn segment(
    State(app): State<Arc<App>>,
    Path((id, file)): Path<(String, String)>,
) -> Result<Response> {
    let s = app.session(&id)?;
    let sequence = file
        .strip_suffix(if s.video.header.is_empty() {
            ".ts"
        } else {
            ".mkv"
        })
        .and_then(|v| v.parse::<i64>().ok())
        .ok_or_else(|| ApiError::new(404, "Segment does not exist"))?;
    // Explicit player reads must also complete while the user is paused: mpv
    // fills its own bounded forward cache in that state. Pause only suppresses
    // unsolicited server prefetch below, never HTTP backpressure itself.
    let mut result = app.segment(&s, sequence)?;
    let data = loop {
        if let Some(value) = result.borrow_and_update().clone() {
            break value?;
        }
        result
            .changed()
            .await
            .map_err(|_| ApiError::new(503, "GPU worker stopped"))?;
    };
    // Queue exactly one segment ahead while this response is being transmitted.
    // This overlaps GPU work with high-latency NetBird transfer without unbounded work.
    if !*s.paused.borrow() {
        let _ = app.segment(&s, sequence + 1);
    }
    Ok(Response::builder()
        .header(
            "content-type",
            if s.video.header.is_empty() {
                "video/MP2T"
            } else {
                "video/x-matroska"
            },
        )
        .header("cache-control", "private, max-age=60")
        .header("x-modeld-frames", data.frames.to_string())
        .header("x-modeld-render-seconds", format!("{:.6}", data.seconds))
        .body(Body::from(data.bytes.clone()))
        .unwrap())
}
fn router(app: Arc<App>) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/v1/models", get(models))
        .route("/v1/sessions", post(create))
        .route("/v1/sessions/{id}", axum::routing::delete(remove))
        .route("/v1/sessions/{id}/heartbeat", post(heartbeat))
        .route("/v1/sessions/{id}/index.m3u8", get(index))
        .route("/v1/sessions/{id}/init.mkv", get(init_segment))
        .route("/v1/sessions/{id}/segments/{file}", get(segment))
        .layer(DefaultBodyLimit::max(65536))
        .with_state(app)
}

#[tokio::main(flavor = "multi_thread", worker_threads = 4)]
async fn main() -> std::result::Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).is_some_and(|s| s == "--help") {
        println!("modeld [--config /etc/modeld/server.json]");
        return Ok(());
    }
    let path = if args.len() == 1 {
        "/etc/modeld/server.json"
    } else if args.len() == 3 && args[1] == "--config" {
        &args[2]
    } else {
        return Err("Usage: modeld [--config FILE]".into());
    };
    let config: Config = serde_json::from_slice(&std::fs::read(path)?)?;
    config.validate()?;
    let token = std::fs::read_to_string(&config.token_file)?
        .trim()
        .to_string();
    if token.len() < 32 {
        return Err("API token must contain at least 32 characters".into());
    }
    let backend = Backend::new(&config.model_path, &config.cache_dir)?;
    let (jobs, mut work) = mpsc::channel::<Job>(config.queue_capacity);
    let worker = std::thread::Builder::new()
        .name("modeld-gpu".into())
        .spawn(move || {
            while let Some(job) = work.blocking_recv() {
                let value = if job.closed.load(Ordering::Acquire) {
                    Err(ApiError::new(410, "Session closed"))
                } else {
                    job.video.render(job.sequence).map(Arc::new)
                };
                if let Err(ref error) = value {
                    eprintln!("[modeld] render: {error}")
                }
                job.result.send_replace(Some(value));
            }
        })?;
    let app = Arc::new(App {
        permits: Arc::new(Semaphore::new(config.max_sessions)),
        config,
        token,
        backend,
        sessions: Mutex::new(HashMap::new()),
        jobs,
    });
    let weak = Arc::downgrade(&app);
    tokio::spawn(async move {
        let mut tick = tokio::time::interval(Duration::from_secs(10));
        loop {
            tick.tick().await;
            if let Some(app) = weak.upgrade() {
                app.reap()
            } else {
                break;
            }
        }
    });
    let address = format!("{}:{}", app.config.listen, app.config.port);
    let listener = tokio::net::TcpListener::bind(&address).await?;
    eprintln!("[modeld] Rust API listening on {address}; RIFE Heavy FP32, TF32 disabled");
    let shutdown_app = app.clone();
    axum::serve(listener, router(app.clone()))
        .with_graceful_shutdown(async move {
            let mut term =
                tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()).unwrap();
            tokio::select! {_=tokio::signal::ctrl_c()=>{},_=term.recv()=>{}}
            shutdown_app.cancel_all();
        })
        .await?;
    drop(app);
    let _ = worker.join();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn finite_timeline_preserves_last_frame() {
        let p = playlist(4.01, 2).unwrap();
        assert!(p.contains("#EXTINF:0.016667,"));
        assert!(p.contains("segments/2.ts"));
        assert!(!p.contains("segments/3.ts"));
        assert!(p.ends_with("#EXT-X-ENDLIST\n"));
        assert!(playlist(f64::NAN, 2).is_err());
        assert!(playlist(-1.0, 2).is_err());
        let main = media_playlist(4.01, 2, true).unwrap();
        assert!(main.contains("#EXT-X-MAP:URI=\"init.mkv\""));
        assert!(main.contains("#EXTINF:0.016667,\nsegments/2.mkv"));
        assert!(!main.contains(".ts"));
    }
    #[test]
    fn unsupported_tasks_are_not_advertised() {
        let data = catalog();
        assert_eq!(data["data"].as_array().unwrap().len(), 1);
        assert_eq!(data["data"][0]["precision"], "fp32");
        assert_eq!(data["data"][0]["tf32"], false);
        let mut body = CreateSession {
            source_url: "http://100.123.1.2:9000/source/token".into(),
            source_backups: Vec::new(),
            source_headers: BTreeMap::new(),
            model: None,
            task: Some("llm".into()),
            target_fps: None,
            ..Default::default()
        };
        assert_eq!(body.validate().unwrap_err().status, 501);
        body.task = None;
        body.target_fps = Some(120);
        assert_eq!(body.validate().unwrap_err().status, 422);
    }
    #[test]
    fn source_protocols_and_credentials_are_restricted() {
        assert!(source_url("file:///etc/passwd").is_err());
        assert!(source_url("http://user:secret@host/video").is_err());
        assert!(source_url("http://100.123.1.2:1234/source/opaque").is_ok());
        let mut body: CreateSession = serde_json::from_value(json!({
            "source_url":"https://cdn.example/video.m4s", "output_mode":"main",
            "audio_source_url":"https://cdn.example/audio.m4s"
        }))
        .unwrap();
        assert!(body.validate().is_ok());
        body.audio_source_url = Some("file:///etc/passwd".into());
        assert!(body.validate().is_err());
        body.audio_source_url = None;
        body.output_mode = Some("unknown".into());
        assert!(body.validate().is_err());
    }
    #[test]
    fn source_headers_preserve_referer_without_request_injection() {
        let mut body: CreateSession = serde_json::from_value(json!({
            "source_url":"https://cdn.example/video.m4s",
            "source_headers":{"Referer":"https://www.bilibili.com/", "User-Agent":"mpv-test"}
        }))
        .unwrap();
        assert!(body.validate().is_ok());
        let headers = body.http_headers().unwrap();
        assert!(headers.contains("Referer: https://www.bilibili.com/\r\n"));
        body.source_headers
            .insert("Cookie".into(), "session=test\r\nHost: private".into());
        assert!(body.validate().is_err());
        body.source_headers.remove("Cookie");
        body.source_headers
            .insert("Host".into(), "other.example".into());
        assert!(body.validate().is_err());
        body.source_headers.remove("Host");
        body.source_headers
            .insert("referer".into(), "https://other.example/".into());
        assert!(body.validate().is_err());
        let old: CreateSession =
            serde_json::from_value(json!({"source_url":"https://cdn.example/video"})).unwrap();
        assert!(old.validate().is_ok());
        assert_eq!(old.http_headers().unwrap(), "");
        let mut fallback: CreateSession = serde_json::from_value(json!({
            "source_url":"https://cdn.example/video.m4s",
            "source_backups":["https://backup.example/video.m4s"]
        }))
        .unwrap();
        assert!(fallback.validate().is_ok());
        fallback.source_backups.push("file:///etc/passwd".into());
        assert!(fallback.validate().is_err());
        fallback.source_backups = vec!["https://backup.example/video.m4s".into(); 9];
        assert!(fallback.validate().is_err());
    }
    #[test]
    fn unpredictable_capability_ids() {
        let a = random_id().unwrap();
        assert_eq!(a.len(), 32);
        assert_ne!(a, random_id().unwrap());
        assert!(a.bytes().all(|b| b.is_ascii_hexdigit()));
    }
}
