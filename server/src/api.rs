use crate::{
    config::Config,
    engine::Engine,
    error::{ApiError, Result},
    native::Cancellation,
    scheduler::{self, Scheduler},
    session::{Session, Sessions},
};
use axum::{
    body::Body,
    extract::{rejection::JsonRejection, DefaultBodyLimit, Path, Request, State},
    http::StatusCode,
    middleware::{self, Next},
    response::{IntoResponse, Response},
    routing::{get, put},
    Json, Router,
};
use modeld_protocol::{CreateSession, Lease, SessionCreated, MODEL, VERSION};
use std::{
    sync::Arc,
    time::{Duration, Instant},
};
use subtle::ConstantTimeEq;

pub struct App {
    pub config: Config,
    token: Vec<u8>,
    pub engine: Arc<dyn Engine>,
    pub sessions: Sessions,
    pub scheduler: Arc<Scheduler>,
}
impl App {
    pub fn new(config: Config, token: String, engine: Arc<dyn Engine>) -> Arc<Self> {
        Arc::new(Self {
            token: format!("Bearer {token}").into_bytes(),
            engine,
            sessions: Sessions::new(config.limits.sessions),
            scheduler: Scheduler::new(config.limits.queued_segments, config.limits.cached_segments),
            config,
        })
    }
}
async fn authorize(State(app): State<Arc<App>>, request: Request, next: Next) -> Response {
    let value = request
        .headers()
        .get("authorization")
        .map(|v| v.as_bytes())
        .unwrap_or_default();
    if !bool::from(value.ct_eq(&app.token)) {
        return ApiError::new(401, "unauthorized", "Bearer token required").into_response();
    }
    next.run(request).await
}
fn body<T>(value: std::result::Result<Json<T>, JsonRejection>) -> Result<T> {
    value
        .map(|Json(v)| v)
        .map_err(|e| ApiError::new(e.status().as_u16(), "invalid_request", e.body_text()))
}
fn random_id() -> Result<String> {
    let mut bytes = [0u8; 16];
    getrandom::fill(&mut bytes)
        .map_err(|_| ApiError::new(500, "random_failed", "Secure random generation failed"))?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}
async fn health(State(app): State<Arc<App>>) -> Result<Json<serde_json::Value>> {
    if !app.scheduler.status().ready {
        return Err(ApiError::new(
            503,
            "scheduler_stopped",
            "GPU scheduler stopped",
        ));
    }
    Ok(Json(
        serde_json::json!({"status":"ready","version":env!("CARGO_PKG_VERSION"),"protocol":VERSION}),
    ))
}
async fn models() -> Json<serde_json::Value> {
    Json(modeld_protocol::models())
}
async fn status(State(app): State<Arc<App>>) -> Json<serde_json::Value> {
    Json(
        serde_json::json!({"version":env!("CARGO_PKG_VERSION"),"protocol":VERSION,
        "scheduler":app.scheduler.status(),"sessions":{
            "limit":app.config.limits.sessions,
            "occupied":app.config.limits.sessions-app.sessions.slots.available_permits(),
            "published":app.sessions.list().len()}}),
    )
}
async fn list(State(app): State<Arc<App>>) -> Json<serde_json::Value> {
    Json(serde_json::json!({"sessions":app.sessions.list()}))
}
async fn inspect(
    State(app): State<Arc<App>>,
    Path(id): Path<String>,
) -> Result<Json<serde_json::Value>> {
    Ok(Json(app.sessions.get(&id)?.status()))
}
struct Probe(Option<Arc<Cancellation>>);
impl Drop for Probe {
    fn drop(&mut self) {
        if let Some(c) = &self.0 {
            c.cancel();
        }
    }
}
async fn create(
    State(app): State<Arc<App>>,
    request: std::result::Result<Json<CreateSession>, JsonRejection>,
) -> Result<(StatusCode, Json<SessionCreated>)> {
    let request = body(request)?;
    if request.model != MODEL {
        return Err(ApiError::new(404, "unknown_model", "Unknown model"));
    }
    request.validate().map_err(ApiError::invalid)?;
    app.sessions.reap();
    let permit = app
        .sessions
        .slots
        .clone()
        .try_acquire_owned()
        .map_err(|_| ApiError::new(429, "session_limit", "All session slots are occupied"))?;
    let cancellation = Cancellation::new()?;
    let mut guard = Probe(Some(cancellation.clone()));
    let engine = app.engine.clone();
    let config = app.config.clone();
    let timeout = config.limits.source_timeout_seconds as u64 + 2;
    let started = Instant::now();
    let work = tokio::task::spawn_blocking(move || {
        engine
            .open(&request, &config, cancellation)
            .map(|stream| (stream, permit))
    });
    let (stream, permit) = tokio::time::timeout(Duration::from_secs(timeout), work)
        .await
        .map_err(|_| ApiError::new(504, "source_timeout", "Source probe exceeded its deadline"))?
        .map_err(|_| ApiError::new(500, "probe_failed", "Source probe worker failed"))??;
    let id = random_id()?;
    let session = Session::new(id.clone(), stream, permit, app.config.limits.lease_seconds);
    let response = SessionCreated {
        id,
        lease_seconds: app.config.limits.lease_seconds,
        manifest: session.manifest(),
    };
    app.sessions.insert(session);
    guard.0 = None;
    eprintln!(
        "{}",
        serde_json::json!({"event":"session.created",
        "elapsed_ms":started.elapsed().as_millis(),"width":response.manifest.media.width,
        "height":response.manifest.media.height,"source_fps":response.manifest.media.source_fps})
    );
    Ok((StatusCode::CREATED, Json(response)))
}
async fn close(State(app): State<Arc<App>>, Path(id): Path<String>) -> StatusCode {
    app.sessions.remove(&id);
    StatusCode::NO_CONTENT
}
async fn lease(
    State(app): State<Arc<App>>,
    Path(id): Path<String>,
    value: std::result::Result<Json<Lease>, JsonRejection>,
) -> Result<Json<serde_json::Value>> {
    let value = body(value)?;
    app.sessions.get(&id)?.renew(value.paused);
    Ok(Json(
        serde_json::json!({"lease_seconds":app.config.limits.lease_seconds,"paused":value.paused}),
    ))
}
async fn manifest(
    State(app): State<Arc<App>>,
    Path(id): Path<String>,
) -> Result<Json<modeld_protocol::Manifest>> {
    let s = app.sessions.get(&id)?;
    s.touch();
    Ok(Json(s.manifest()))
}
async fn initialization(State(app): State<Arc<App>>, Path(id): Path<String>) -> Result<Response> {
    let s = app.sessions.get(&id)?;
    s.touch();
    Ok(Response::builder()
        .header("content-type", "video/x-matroska")
        .header("cache-control", "private, max-age=3600")
        .body(Body::from(s.stream.initialization().as_ref().clone()))
        .unwrap())
}
async fn segment(
    State(app): State<Arc<App>>,
    Path((id, index)): Path<(String, String)>,
) -> Result<Response> {
    let index = index
        .parse::<u64>()
        .map_err(|_| ApiError::invalid("Segment index must be an unsigned integer"))?;
    let s = app.sessions.get(&id)?;
    let result = scheduler::wait(app.scheduler.request(&s, index)?).await?;
    // Demand reads complete while user-paused; pause only suppresses extra work.
    if !s.paused() {
        let _ = app.scheduler.request(&s, index.saturating_add(1));
    }
    Ok(Response::builder()
        .header("content-type", "video/x-matroska")
        .header("cache-control", "private, max-age=60")
        .header("x-modeld-frames", result.frames.to_string())
        .header(
            "x-modeld-render-seconds",
            format!("{:.6}", result.render_seconds),
        )
        .body(Body::from(result.bytes.clone()))
        .unwrap())
}
pub fn router(app: Arc<App>) -> Router {
    let control = Router::new()
        .route("/api/v2/models", get(models))
        .route("/api/v2/status", get(status))
        .route("/api/v2/sessions", get(list).post(create))
        .route("/api/v2/sessions/{id}", get(inspect).delete(close))
        .route("/api/v2/sessions/{id}/lease", put(lease))
        .route_layer(middleware::from_fn_with_state(app.clone(), authorize));
    Router::new()
        .route("/healthz", get(health))
        .merge(control)
        .route("/api/v2/sessions/{id}/media", get(manifest))
        .route("/api/v2/sessions/{id}/media/init", get(initialization))
        .route("/api/v2/sessions/{id}/media/segments/{index}", get(segment))
        .fallback(|| async { ApiError::missing() })
        .method_not_allowed_fallback(|| async {
            ApiError::new(405, "method_not_allowed", "Method not allowed")
        })
        .layer(DefaultBodyLimit::max(65536))
        .with_state(app)
}
