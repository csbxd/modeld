use axum::{
    http::StatusCode,
    response::{IntoResponse, Response},
    Json,
};
use serde::Serialize;
use std::fmt;

pub type Result<T> = std::result::Result<T, ApiError>;
#[derive(Debug, Clone, Serialize)]
pub struct ApiError {
    #[serde(skip)]
    pub status: u16,
    pub code: &'static str,
    pub message: String,
    pub retryable: bool,
}
impl ApiError {
    pub fn new(status: u16, code: &'static str, message: impl Into<String>) -> Self {
        Self {
            status,
            code,
            message: message.into(),
            retryable: matches!(status, 408 | 429 | 502 | 503 | 504),
        }
    }
    pub fn invalid(message: impl Into<String>) -> Self {
        Self::new(422, "invalid_request", message)
    }
    pub fn missing() -> Self {
        Self::new(404, "not_found", "Resource does not exist")
    }
    pub fn closed() -> Self {
        Self::new(410, "session_closed", "Session is closed")
    }
    pub fn native(status: u16, message: impl Into<String>) -> Self {
        let code = match status {
            410 => "session_closed",
            422 => "unsupported_media",
            499 => "canceled",
            504 => "source_timeout",
            502 => "source_failed",
            _ => "backend_failed",
        };
        Self::new(status, code, message)
    }
}
impl fmt::Display for ApiError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.code, self.message)
    }
}
impl std::error::Error for ApiError {}
impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (
            StatusCode::from_u16(self.status).unwrap_or(StatusCode::INTERNAL_SERVER_ERROR),
            Json(serde_json::json!({"error":self})),
        )
            .into_response()
    }
}
