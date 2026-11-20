use crate::{
    api::{self, App},
    config::{BrowserCodec, Config},
    engine::{Engine, Segment, Stream},
    error::{ApiError, Result},
    native::Cancellation,
    scheduler,
};
use axum::{
    body::{to_bytes, Body},
    http::{Request, StatusCode},
    Router,
};
use modeld_protocol::{CreateSession, Media, MODEL};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Condvar, Mutex,
    },
    time::{Duration, Instant},
};
use tower::ServiceExt;
const TOKEN: &str = "unit-test-token-not-a-live-credential";
#[derive(Default)]
struct Gate {
    open: Mutex<bool>,
    condition: Condvar,
}
impl Gate {
    fn release(&self) {
        *self.open.lock().unwrap() = true;
        self.condition.notify_all();
    }
}
#[derive(Default)]
struct Fake {
    calls: Arc<Mutex<BTreeMap<u64, usize>>>,
    canceled: Arc<AtomicUsize>,
    probe_block: AtomicBool,
    probe_started: AtomicBool,
    canceled_probes: AtomicUsize,
    gate: Option<Arc<Gate>>,
}
struct FakeStream {
    browser: Option<BrowserCodec>,
    media: Media,
    calls: Arc<Mutex<BTreeMap<u64, usize>>>,
    canceled: Arc<AtomicUsize>,
    closed: AtomicBool,
    gate: Option<Arc<Gate>>,
}
impl Engine for Fake {
    fn open(
        self: Arc<Self>,
        _: &CreateSession,
        config: &Config,
        cancellation: Arc<Cancellation>,
    ) -> Result<Arc<dyn Stream>> {
        if self.probe_block.load(Ordering::Acquire) {
            self.probe_started.store(true, Ordering::Release);
            while !cancellation.canceled() {
                std::thread::sleep(Duration::from_millis(1));
            }
            self.canceled_probes.fetch_add(1, Ordering::Relaxed);
            return Err(ApiError::new(499, "canceled", "Probe canceled"));
        }
        Ok(Arc::new(FakeStream {
            browser: config.output.browser,
            media: Media {
                width: 320,
                height: 180,
                duration_seconds: 12.,
                source_fps: 30.,
                output_fps: 60,
                segment_seconds: 2,
                segment_count: 6,
                tracks: vec![],
            },
            calls: self.calls.clone(),
            canceled: self.canceled.clone(),
            closed: AtomicBool::new(false),
            gate: self.gate.clone(),
        }))
    }
}
impl Stream for FakeStream {
    fn content_type(&self) -> &str {
        match self.browser {
            Some(BrowserCodec::H264) => "video/mp4; codecs=\"avc1.64002A,mp4a.40.2\"",
            Some(BrowserCodec::Av1) => "video/mp4; codecs=\"av01.0.09M.08,mp4a.40.2\"",
            None => "video/x-matroska",
        }
    }
    fn media(&self) -> &Media {
        &self.media
    }
    fn initialization(&self) -> Arc<Vec<u8>> {
        Arc::new(b"matroska-init".to_vec())
    }
    fn cancel(&self) {
        if !self.closed.swap(true, Ordering::AcqRel) {
            self.canceled.fetch_add(1, Ordering::Relaxed);
        }
        if let Some(gate) = &self.gate {
            gate.release();
        }
    }
    fn render(&self, index: u64) -> Result<Segment> {
        *self.calls.lock().unwrap().entry(index).or_default() += 1;
        if let Some(gate) = &self.gate {
            let mut open = gate.open.lock().unwrap();
            while !*open {
                open = gate.condition.wait(open).unwrap();
            }
        }
        if self.closed.load(Ordering::Acquire) {
            return Err(ApiError::closed());
        }
        Ok(Segment {
            bytes: format!("cluster-{index}").into_bytes(),
            frames: 120,
            render_seconds: 0.01,
        })
    }
}
fn config() -> Config {
    serde_json::from_value(json!({"listen":"127.0.0.1:8765","token_file":"/test-token",
        "inference":{"model_file":"/model.onnx","cache_directory":"/cache"},
        "limits":{"sessions":1,"queued_segments":1,"cached_segments":2}}))
    .unwrap()
}
fn body() -> Value {
    json!({"model":MODEL,"video":{"urls":["http://source.example/video"]}})
}
async fn request(
    router: Router,
    method: &str,
    path: &str,
    body: Option<Value>,
    authenticated: bool,
) -> (StatusCode, Vec<u8>) {
    let mut req = Request::builder().method(method).uri(path);
    if authenticated {
        req = req.header("Authorization", format!("Bearer {TOKEN}"));
    }
    if body.is_some() {
        req = req.header("Content-Type", "application/json");
    }
    let response = router
        .oneshot(
            req.body(body.map_or_else(Body::empty, |v| Body::from(v.to_string())))
                .unwrap(),
        )
        .await
        .unwrap();
    let status = response.status();
    (
        status,
        to_bytes(response.into_body(), 4 * 1024 * 1024)
            .await
            .unwrap()
            .to_vec(),
    )
}
async fn json_request(
    router: Router,
    method: &str,
    path: &str,
    body: Option<Value>,
    authenticated: bool,
) -> (u16, Value) {
    let (status, bytes) = request(router, method, path, body, authenticated).await;
    (
        status.as_u16(),
        if bytes.is_empty() {
            Value::Null
        } else {
            serde_json::from_slice(&bytes).unwrap()
        },
    )
}
async fn wait_until(check: impl Fn() -> bool) {
    let start = Instant::now();
    while !check() {
        assert!(start.elapsed() < Duration::from_secs(2));
        tokio::time::sleep(Duration::from_millis(2)).await;
    }
}
#[tokio::test]
async fn routes_authentication_and_json_contract_are_explicit() {
    let app = App::new(config(), TOKEN.into(), Arc::new(Fake::default()));
    let router = api::router(app.clone());
    assert_eq!(
        json_request(router.clone(), "GET", "/healthz", None, false)
            .await
            .1["protocol"],
        2
    );
    for path in ["/health", "/v1/models", "/v1/sessions"] {
        let (status, value) = json_request(router.clone(), "GET", path, None, true).await;
        assert_eq!(status, 404);
        assert_eq!(value["error"]["code"], "not_found");
    }
    let (status, value) = json_request(router.clone(), "GET", "/api/v2/status", None, false).await;
    assert_eq!(status, 401);
    assert_eq!(value["error"]["code"], "unauthorized");
    let (status, value) = json_request(router.clone(), "GET", "/api/v2/models", None, true).await;
    assert_eq!(status, 200);
    assert_eq!(value["models"][0]["precision"], "fp32");
    let mut bad = body();
    bad["output_mode"] = "video".into();
    let (status, value) =
        json_request(router.clone(), "POST", "/api/v2/sessions", Some(bad), true).await;
    assert_eq!(status, 422);
    assert_eq!(value["error"]["code"], "invalid_request");
    let (status, value) = json_request(router, "POST", "/api/v2/models", None, true).await;
    assert_eq!(status, 405);
    assert_eq!(value["error"]["code"], "method_not_allowed");
}
#[tokio::test]
async fn paused_demand_cache_lifetime_and_media_capability() {
    let fake = Arc::new(Fake::default());
    let app = App::new(config(), TOKEN.into(), fake.clone());
    let router = api::router(app.clone());
    let (status, created) = json_request(
        router.clone(),
        "POST",
        "/api/v2/sessions",
        Some(body()),
        true,
    )
    .await;
    assert_eq!(status, 201);
    let id = created["id"].as_str().unwrap();
    assert_eq!(id.len(), 32);
    let base = format!("/api/v2/sessions/{id}");
    assert_eq!(created["manifest"]["schema"], 2);
    assert_eq!(
        json_request(router.clone(), "GET", &base, None, false)
            .await
            .0,
        401
    );
    assert_eq!(
        json_request(router.clone(), "GET", &format!("{base}/media"), None, false)
            .await
            .0,
        200
    );
    assert_eq!(
        json_request(
            router.clone(),
            "PUT",
            &format!("{base}/lease"),
            Some(json!({"paused":true})),
            true
        )
        .await
        .0,
        200
    );
    for _ in 0..2 {
        let (status, bytes) = request(
            router.clone(),
            "GET",
            &format!("{base}/media/segments/0"),
            None,
            false,
        )
        .await;
        assert_eq!(status, 200);
        assert_eq!(bytes, b"cluster-0");
    }
    assert_eq!(*fake.calls.lock().unwrap(), BTreeMap::from([(0, 1)]));
    let (status, bytes) = request(
        router.clone(),
        "GET",
        &format!("{base}/media/init"),
        None,
        false,
    )
    .await;
    assert_eq!(status, 200);
    assert_eq!(bytes, b"matroska-init");
    let (_, state) = json_request(router.clone(), "GET", &base, None, true).await;
    assert_eq!(state["progress"]["last_completed"], 0);
    assert_eq!(state["paused"], true);
    assert_eq!(
        json_request(
            router.clone(),
            "GET",
            &format!("{base}/media/segments/0.ts"),
            None,
            false
        )
        .await
        .0,
        422
    );
    assert_eq!(
        json_request(router.clone(), "DELETE", &base, None, true)
            .await
            .0,
        204
    );
    assert_eq!(fake.canceled.load(Ordering::Relaxed), 1);
    assert_eq!(
        json_request(router.clone(), "GET", &format!("{base}/media"), None, false)
            .await
            .0,
        404
    );
    assert_eq!(
        json_request(router, "DELETE", &base, None, true).await.0,
        204
    );
}
#[tokio::test]
async fn queue_limits_deduplicate_pending_work_and_retain_recent_segments() {
    let gate = Arc::new(Gate::default());
    let fake = Arc::new(Fake {
        gate: Some(gate.clone()),
        ..Default::default()
    });
    let app = App::new(config(), TOKEN.into(), fake.clone());
    let router = api::router(app.clone());
    let (_, created) = json_request(
        router.clone(),
        "POST",
        "/api/v2/sessions",
        Some(body()),
        true,
    )
    .await;
    let session = app.sessions.get(created["id"].as_str().unwrap()).unwrap();
    let first = app.scheduler.request(&session, 0).unwrap();
    wait_until(|| fake.calls.lock().unwrap().get(&0) == Some(&1)).await;
    let duplicate = app.scheduler.request(&session, 0).unwrap();
    let second = app.scheduler.request(&session, 1).unwrap();
    assert_eq!(
        app.scheduler.request(&session, 2).unwrap_err().code,
        "queue_full"
    );
    assert_eq!(
        json_request(router, "POST", "/api/v2/sessions", Some(body()), true)
            .await
            .0,
        429
    );
    gate.release();
    scheduler::wait(first).await.unwrap();
    scheduler::wait(duplicate).await.unwrap();
    scheduler::wait(second).await.unwrap();
    assert_eq!(fake.calls.lock().unwrap().get(&0), Some(&1));
    scheduler::wait(app.scheduler.request(&session, 0).unwrap())
        .await
        .unwrap();
    scheduler::wait(app.scheduler.request(&session, 2).unwrap())
        .await
        .unwrap();
    scheduler::wait(app.scheduler.request(&session, 0).unwrap())
        .await
        .unwrap();
    assert_eq!(fake.calls.lock().unwrap().get(&0), Some(&1));
    scheduler::wait(app.scheduler.request(&session, 1).unwrap())
        .await
        .unwrap();
    assert_eq!(fake.calls.lock().unwrap().get(&1), Some(&2));
}
#[tokio::test]
async fn disconnecting_probe_cancels_native_work_and_releases_its_slot() {
    let fake = Arc::new(Fake {
        probe_block: AtomicBool::new(true),
        ..Default::default()
    });
    let app = App::new(config(), TOKEN.into(), fake.clone());
    let router = api::router(app.clone());
    let work = tokio::spawn(json_request(
        router.clone(),
        "POST",
        "/api/v2/sessions",
        Some(body()),
        true,
    ));
    wait_until(|| fake.probe_started.load(Ordering::Acquire)).await;
    assert_eq!(app.sessions.slots.available_permits(), 0);
    work.abort();
    let _ = work.await;
    wait_until(|| {
        fake.canceled_probes.load(Ordering::Acquire) == 1
            && app.sessions.slots.available_permits() == 1
    })
    .await;
    fake.probe_block.store(false, Ordering::Release);
    assert_eq!(
        json_request(router, "POST", "/api/v2/sessions", Some(body()), true)
            .await
            .0,
        201
    );
}
#[tokio::test]
async fn browser_profile_has_its_own_authenticated_contract() {
    let app = App::new(config(), TOKEN.into(), Arc::new(Fake::default()));
    let router = api::router(app.clone());
    assert_eq!(
        json_request(
            router.clone(),
            "GET",
            "/api/v2/browser/capabilities",
            None,
            false
        )
        .await
        .0,
        401
    );
    let (_, capabilities) = json_request(
        router.clone(),
        "GET",
        "/api/v2/browser/capabilities",
        None,
        true,
    )
    .await;
    assert_eq!(capabilities["video_codec"], "h264");
    assert_eq!(capabilities["video_codecs"], json!(["h264", "av1"]));
    for (codec, qp) in [("h264", 41), ("av1", 256), ("hevc", 18)] {
        let mut request = body();
        request["video_codec"] = codec.into();
        request["qp"] = qp.into();
        assert_eq!(
            json_request(
                router.clone(),
                "POST",
                "/api/v2/browser/sessions",
                Some(request),
                true,
            )
            .await
            .0,
            422
        );
    }
    let mut av1 = body();
    av1["video_codec"] = "av1".into();
    av1["qp"] = 45.into();
    for (request, mime) in [
        (body(), "video/mp4; codecs=\"avc1.64002A,mp4a.40.2\""),
        (av1, "video/mp4; codecs=\"av01.0.09M.08,mp4a.40.2\""),
    ] {
        let (status, created) = json_request(
            router.clone(),
            "POST",
            "/api/v2/browser/sessions",
            Some(request),
            true,
        )
        .await;
        assert_eq!(status, 201);
        assert_eq!(created["mime_type"], mime);
        let id = created["id"].as_str().unwrap();
        assert_eq!(
            json_request(
                router.clone(),
                "DELETE",
                &format!("/api/v2/sessions/{id}"),
                None,
                true,
            )
            .await
            .0,
            204
        );
    }
    assert!(
        app.config.output.browser.is_none(),
        "a browser request cannot change the mpv default"
    );
}
