# modeld 2.0 architecture

2.0 has one media output: interpolated video, copied audio and copied subtitles/attachments in Matroska. The HTTP API uses typed JSON manifests and relative media paths. The mpv adapter constructs an EDL timeline with a shared initialization prefix. All A/V belongs to mpv's main demuxer; native cache waiting controls the clock. The adapter never writes the user's pause property.

The v1 routes, MPEG-TS output, output mode selection, HLS manifest parsing, old parameter aliases and absent-native-backend production build have been removed. Version history and old validation remain available in Git history. GPU inference remains the verified RIFE/CUDA implementation; orchestration and public contracts are redesigned.

## Boundaries

| Module | Responsibility |
|---|---|
| `protocol/` | Strict request/response types, validation, capabilities and timeline arithmetic; builds without CUDA |
| `server/src/api.rs` | HTTP routing, authentication, extraction and responses |
| `server/src/session.rs` | Session slots, activity leases, cancellation, progress and per-session LRU cache |
| `server/src/scheduler.rs` | Bounded, deduplicated segment jobs on one GPU worker |
| `server/src/engine.rs` | Renderer interface; tests use an isolated in-memory renderer |
| `server/src/native.rs` | Owned FFI handles and conversion to protocol types; production always uses the native backend |
| `src/native_api.cpp` | C ABI boundary, cancellation tokens and resolution-profile ownership |
| `src/media.cpp` | HTTP sources, decoding, RIFE/NVENC and original audio/subtitle muxing |
| `src/rife.cpp`, `src/kernels.cu` | FP32 inference and GPU pixel processing |
| `src/config.cpp`, `src/api.cpp` | Typed client configuration, transport, IP pinning and API errors |
| `src/protocol.cpp` | Client contract validation and EDL construction |
| `src/session.cpp` | Asynchronous opening, RAII closure and a background lease worker |
| `src/media_proxy.cpp` | Loopback stream, cancellable prefetch and byte-bounded cache |
| `src/mpv_state.cpp` | mpv values, source snapshot, track mapping, chapters and anonymous subtitle snapshots |
| `src/mpv_plugin.cpp` | Explicit original/opening/loading/processed/restoring/refreshing state machine |

## Invariants

- Source inputs are HTTP(S) URL lists with their own validated headers. Separate audio can have independent credentials and backup URLs.
- A session slot remains occupied until native probing or rendering actually exits. Disconnecting a create request signals cancellation; it does not detach work outside the limit.
- The GPU worker serializes inference. Duplicate requests share one pending result. Pending work is bounded by the global queue; completed results use LRU eviction.
- Leases renew on explicit lease updates and media demand. Observability queries do not keep abandoned sessions alive.
- User-paused demand still completes. Only the server's extra one-segment prefetch is suppressed.
- The client cancels superseded pending transfers on distant seeks and keeps completed segments. Cache size applies to retained bytes; active transfers and HTTP responses need additional temporary memory.
- Control requests require a Bearer token. Media URLs carry an unpredictable session capability, with no credentials embedded in their paths.
- Media paths are relative to the configured API origin. No signed-origin rewriting, public URL guessing or media-host DNS special cases are needed.
- Source failover preserves the exact Bilibili DASH encoding and uses the official returned URLs.
- Reloading the same playlist entry preserves its ID and surrounding entries. Track restoration occurs in `on_preloaded`, before playback begins.
- Temporary subtitle files are copied to anonymous descriptors before reload. Descriptor ownership follows the playback snapshot.
- Additional media models extend the engine/capability boundary. Text generation would have its own request/resource types; it is not represented as a video session.

## Configuration and deployment

Both configuration files reject unknown fields. A deployment migrates the operator's endpoint, credentials, bind address and limits once, then installs matching 2.0 client/server packages. Runtime code does not parse older configuration shapes or negotiate old APIs.

Portage owns programs, SDK/model dependencies, service definitions and examples. Credentials, NetBird addresses and resource limits remain deployment configuration; cached engines and media are runtime data. The mpv executable is unchanged.
