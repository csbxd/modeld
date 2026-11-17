use crate::{
    error::{ApiError, Result},
    session::{CacheEntry, SegmentResult, Session},
};
use serde::Serialize;
use std::{
    sync::{
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
        Arc, Mutex,
    },
    thread,
};
use tokio::sync::{mpsc, watch};

struct Job {
    session: Arc<Session>,
    index: u64,
    result: watch::Sender<SegmentResult>,
}
#[derive(Default)]
struct Counters {
    running: AtomicUsize,
    completed: AtomicU64,
    failed: AtomicU64,
}
#[derive(Serialize)]
pub struct Status {
    pub queued: usize,
    pub queue_capacity: usize,
    pub rendering: usize,
    pub completed: u64,
    pub failed: u64,
    pub ready: bool,
}
pub struct Scheduler {
    sender: Mutex<Option<mpsc::Sender<Job>>>,
    worker: Mutex<Option<thread::JoinHandle<()>>>,
    counters: Arc<Counters>,
    alive: Arc<AtomicBool>,
    capacity: usize,
}
impl Scheduler {
    pub fn new(capacity: usize, cached_segments: usize) -> Arc<Self> {
        let (sender, mut receiver) = mpsc::channel::<Job>(capacity);
        let counters = Arc::new(Counters::default());
        let alive = Arc::new(AtomicBool::new(true));
        let stats = counters.clone();
        let ready = alive.clone();
        let worker = thread::Builder::new()
            .name("modeld-gpu".into())
            .spawn(move || {
                struct Exit(Arc<AtomicBool>);
                impl Drop for Exit {
                    fn drop(&mut self) {
                        self.0.store(false, Ordering::Release);
                    }
                }
                let _exit = Exit(ready);
                while let Some(job) = receiver.blocking_recv() {
                    let s = &job.session;
                    stats.running.store(1, Ordering::Release);
                    s.progress.lock().unwrap().rendering = Some(job.index);
                    let result = if s.closed.load(Ordering::Acquire) {
                        Err(ApiError::closed())
                    } else {
                        s.stream.render(job.index).map(Arc::new)
                    };
                    {
                        let mut progress = s.progress.lock().unwrap();
                        progress.rendering = None;
                        match &result {
                            Ok(_) => {
                                progress.last_completed = Some(job.index);
                                progress.completed_segments += 1;
                                progress.last_error = None;
                                stats.completed.fetch_add(1, Ordering::Relaxed);
                            }
                            Err(e) => {
                                progress.last_error = Some(e.clone());
                                stats.failed.fetch_add(1, Ordering::Relaxed);
                                eprintln!("[modeld] segment {} failed: {e}", job.index);
                            }
                        }
                    }
                    job.result.send_replace(Some(result));
                    s.cache.lock().unwrap().trim(cached_segments);
                    stats.running.store(0, Ordering::Release);
                }
            })
            .expect("Cannot start GPU worker");
        Arc::new(Self {
            sender: Mutex::new(Some(sender)),
            worker: Mutex::new(Some(worker)),
            counters,
            alive,
            capacity,
        })
    }
    pub fn request(&self, s: &Arc<Session>, index: u64) -> Result<watch::Receiver<SegmentResult>> {
        if s.closed.load(Ordering::Acquire) {
            return Err(ApiError::closed());
        }
        if s.stream.media().segment_frames(index).is_none() {
            return Err(ApiError::missing());
        }
        s.touch();
        s.progress.lock().unwrap().last_requested = Some(index);
        let mut cache = s.cache.lock().unwrap();
        cache.clock += 1;
        let tick = cache.clock;
        if let Some(entry) = cache.entries.get_mut(&index) {
            if !matches!(&*entry.result.borrow(), Some(Err(_))) {
                entry.used = tick;
                return Ok(entry.result.clone());
            }
            cache.entries.remove(&index);
        }
        let (sender, receiver) = watch::channel(None);
        let queue = self.sender.lock().unwrap();
        let queue = queue
            .as_ref()
            .ok_or_else(|| ApiError::new(503, "scheduler_stopped", "GPU scheduler stopped"))?;
        queue
            .try_send(Job {
                session: s.clone(),
                index,
                result: sender,
            })
            .map_err(|e| match e {
                mpsc::error::TrySendError::Full(_) => {
                    ApiError::new(429, "queue_full", "GPU queue is full")
                }
                mpsc::error::TrySendError::Closed(_) => {
                    ApiError::new(503, "scheduler_stopped", "GPU scheduler stopped")
                }
            })?;
        cache.entries.insert(
            index,
            CacheEntry {
                result: receiver.clone(),
                used: tick,
            },
        );
        Ok(receiver)
    }
    pub fn status(&self) -> Status {
        let queued = self
            .sender
            .lock()
            .unwrap()
            .as_ref()
            .map_or(0, |s| self.capacity - s.capacity());
        Status {
            queued,
            queue_capacity: self.capacity,
            rendering: self.counters.running.load(Ordering::Acquire),
            completed: self.counters.completed.load(Ordering::Relaxed),
            failed: self.counters.failed.load(Ordering::Relaxed),
            ready: self.alive.load(Ordering::Acquire),
        }
    }
}
impl Drop for Scheduler {
    fn drop(&mut self) {
        self.sender.get_mut().unwrap().take();
        if let Some(worker) = self.worker.get_mut().unwrap().take() {
            let _ = worker.join();
        }
    }
}
pub async fn wait(
    mut receiver: watch::Receiver<SegmentResult>,
) -> Result<Arc<crate::engine::Segment>> {
    loop {
        let value = receiver.borrow_and_update().clone();
        if let Some(value) = value {
            return value;
        }
        receiver
            .changed()
            .await
            .map_err(|_| ApiError::new(503, "scheduler_stopped", "GPU worker stopped"))?;
    }
}
