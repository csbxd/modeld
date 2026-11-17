mod api;
mod config;
mod engine;
mod error;
mod native;
mod scheduler;
mod session;
#[cfg(test)]
mod tests;

use std::{path::PathBuf, sync::Arc, time::Duration};

fn config_path() -> Result<PathBuf, Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    match args.as_slice() {
        [] => Ok("/etc/modeld/server.json".into()),
        [flag, path] if flag == "--config" || flag == "--check-config" => Ok(path.into()),
        _ => Err("Usage: modeld --version | --config FILE | --check-config FILE".into()),
    }
}
#[tokio::main(flavor = "multi_thread", worker_threads = 4)]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    if std::env::args().nth(1).as_deref() == Some("--version") {
        println!("{}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }
    if std::env::args().nth(1).as_deref() == Some("--help") {
        println!("modeld [--config /etc/modeld/server.json] | --check-config FILE | --version");
        return Ok(());
    }
    let config: config::Config = serde_json::from_slice(&std::fs::read(config_path()?)?)?;
    config.validate()?;
    let token = std::fs::read_to_string(&config.token_file)?
        .trim()
        .to_owned();
    if token.len() < 32 {
        return Err("API token must contain at least 32 characters".into());
    }
    if std::env::args().nth(1).as_deref() == Some("--check-config") {
        println!(
            "{}",
            serde_json::json!({"status":"valid","protocol":modeld_protocol::VERSION})
        );
        return Ok(());
    }
    let backend = native::Backend::new(
        &config.inference.model_file,
        &config.inference.cache_directory,
    )?;
    let address = config.listen;
    let app = api::App::new(config, token, backend);
    let weak = Arc::downgrade(&app);
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(5));
        loop {
            interval.tick().await;
            let Some(app) = weak.upgrade() else { break };
            app.sessions.reap();
        }
    });
    let listener = tokio::net::TcpListener::bind(address).await?;
    eprintln!(
        "{}",
        serde_json::json!({"event":"server.ready","version":env!("CARGO_PKG_VERSION"),
        "protocol":modeld_protocol::VERSION,"listen":address.to_string()})
    );
    let shutdown = app.clone();
    axum::serve(listener, api::router(app.clone()))
        .with_graceful_shutdown(async move {
            let mut term =
                tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()).unwrap();
            tokio::select! {_=tokio::signal::ctrl_c()=>{},_=term.recv()=>{}}
            shutdown.sessions.close_all();
        })
        .await?;
    drop(app);
    Ok(())
}
