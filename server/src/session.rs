use crate::{
    engine::{Segment, Stream},
    error::{ApiError, Result},
};
use modeld_protocol::Manifest;
use serde::Serialize;
use std::{
    collections::{BTreeMap, HashMap},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};
use tokio::sync::{watch, OwnedSemaphorePermit, Semaphore};

pub type SegmentResult = Option<Result<Arc<Segment>>>;
pub struct CacheEntry {
    pub result: watch::Receiver<SegmentResult>,
    pub used: u64,
}
#[derive(Default)]
pub struct Cache {
    pub entries: BTreeMap<u64, CacheEntry>,
    pub clock: u64,
}
impl Cache {
    pub fn trim(&mut self, limit: usize) {
        loop {
            let ready: Vec<_> = self
                .entries
                .iter()
                .filter(|(_, e)| e.result.borrow().is_some())
                .collect();
            if ready.len() <= limit {
                break;
            }
            let index = *ready.into_iter().min_by_key(|(_, e)| e.used).unwrap().0;
            self.entries.remove(&index);
        }
    }
}
#[derive(Default, Clone, Serialize)]
pub struct Progress {
    pub rendering: Option<u64>,
    pub last_requested: Option<u64>,
    pub last_completed: Option<u64>,
    pub completed_segments: u64,
    pub last_error: Option<ApiError>,
}
struct Activity {
    touched: Instant,
    paused: bool,
}
pub struct Session {
    pub id: String,
    pub stream: Arc<dyn Stream>,
    pub closed: AtomicBool,
    pub cache: Mutex<Cache>,
    pub progress: Mutex<Progress>,
    activity: Mutex<Activity>,
    lease: Duration,
    _permit: OwnedSemaphorePermit,
}
impl Session {
    pub fn new(
        id: String,
        stream: Arc<dyn Stream>,
        permit: OwnedSemaphorePermit,
        lease: u64,
    ) -> Arc<Self> {
        Arc::new(Self {
            id,
            stream,
            closed: AtomicBool::new(false),
            cache: Mutex::new(Cache::default()),
            progress: Mutex::new(Progress::default()),
            activity: Mutex::new(Activity {
                touched: Instant::now(),
                paused: false,
            }),
            lease: Duration::from_secs(lease),
            _permit: permit,
        })
    }
    pub fn touch(&self) {
        self.activity.lock().unwrap().touched = Instant::now();
    }
    pub fn renew(&self, paused: bool) {
        *self.activity.lock().unwrap() = Activity {
            touched: Instant::now(),
            paused,
        };
    }
    pub fn paused(&self) -> bool {
        self.activity.lock().unwrap().paused
    }
    pub fn expired(&self) -> bool {
        self.activity.lock().unwrap().touched.elapsed() >= self.lease
    }
    pub fn close(&self) {
        if !self.closed.swap(true, Ordering::AcqRel) {
            self.stream.cancel();
        }
    }
    pub fn manifest(&self) -> Manifest {
        Manifest::new(&self.id, self.stream.media().clone())
    }
    pub fn status(&self) -> serde_json::Value {
        let activity = self.activity.lock().unwrap();
        let seconds = self
            .lease
            .saturating_sub(activity.touched.elapsed())
            .as_secs();
        let paused = activity.paused;
        drop(activity);
        let cache = self.cache.lock().unwrap();
        let cached: Vec<_> = cache
            .entries
            .iter()
            .filter_map(|(i, e)| matches!(&*e.result.borrow(), Some(Ok(_))).then_some(*i))
            .collect();
        let pending: Vec<_> = cache
            .entries
            .iter()
            .filter_map(|(i, e)| e.result.borrow().is_none().then_some(*i))
            .collect();
        drop(cache);
        serde_json::json!({"id":self.id,"model":modeld_protocol::MODEL,"paused":paused,
            "lease_remaining_seconds":seconds,"progress":*self.progress.lock().unwrap(),
            "cached_segments":cached,"pending_segments":pending,"media":self.stream.media()})
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.close();
    }
}
pub struct Sessions {
    entries: Mutex<HashMap<String, Arc<Session>>>,
    pub slots: Arc<Semaphore>,
}
impl Sessions {
    pub fn new(limit: usize) -> Self {
        Self {
            entries: Mutex::new(HashMap::new()),
            slots: Arc::new(Semaphore::new(limit)),
        }
    }
    pub fn insert(&self, session: Arc<Session>) {
        self.entries
            .lock()
            .unwrap()
            .insert(session.id.clone(), session);
    }
    pub fn get(&self, id: &str) -> Result<Arc<Session>> {
        self.entries
            .lock()
            .unwrap()
            .get(id)
            .filter(|s| !s.closed.load(Ordering::Acquire))
            .cloned()
            .ok_or_else(ApiError::missing)
    }
    pub fn remove(&self, id: &str) {
        if let Some(session) = self.entries.lock().unwrap().remove(id) {
            session.close();
        }
    }
    pub fn list(&self) -> Vec<serde_json::Value> {
        let mut values: Vec<_> = self.entries.lock().unwrap().values().cloned().collect();
        values.sort_by(|a, b| a.id.cmp(&b.id));
        values.iter().map(|s| s.status()).collect()
    }
    pub fn reap(&self) {
        self.entries.lock().unwrap().retain(|_, s| {
            if s.expired() {
                s.close();
                false
            } else {
                true
            }
        });
    }
    pub fn close_all(&self) {
        for (_, s) in self.entries.lock().unwrap().drain() {
            s.close();
        }
    }
}
impl Drop for Sessions {
    fn drop(&mut self) {
        self.close_all();
    }
}
