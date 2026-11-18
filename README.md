# modeld 2.1

通过远端 GPU 为 mpv 提供 **RIFE 4.26 Heavy FP32** 插帧。控制与调度使用 Rust，视频处理使用 C++/CUDA，mpv 接入使用原生 C 插件。生产程序无需 Python 或 VapourSynth。

2.0 重新设计了协议、会话生命周期和播放器状态管理，只提供完整主媒体：**60 fps 插帧画面 + 原音轨 + 内封字幕**。两端使用同一套 `/api/v2` 接口和 JSON 时间轴，旧 API 与纯视频输出已移除。[接口文档](docs/api.md)、[模块与设计约束](docs/architecture.md)、[真实 GPU 与播放器验证](docs/validation-2.0.0.json)、[两端安装与正式服务验证](docs/validation-2.0.0-installed.json)。

## 浏览器内播放

[浏览器扩展](docs/browser.md)可在 B 站原播放器内补到 60 fps，并提供服务、缓冲、缓存与编码画质设置。安装 `www-plugins/modeld-browser::gentoo-cs`，浏览器扩展会复用本机连接配置。原 mpv 功能继续保留。

## mpv 播放

打开视频后按 **Ctrl+Shift+i** 开启，再按恢复原视频。准备期间原视频继续播放，再次按键可取消。新文件默认关闭插帧，日式 2D 动漫保持原视频即可。

开启后，s8030 读取网站原片源和分离音轨，asa 接收完整处理后的主媒体。音轨、字幕与附件直接复制。mpv 自己处理前向缓存、后向定位与数据不足时的缓冲等待；插件不改动用户暂停状态，也不修改 mpv 源码。

同一文件的已完成插帧分段默认保留 256 MiB，可跨拖动和开关复用。缓存按最近使用顺序回收，离开文件或退出后释放。切换主媒体会重建原始文件的 demux 缓存。播放列表、位置、章节、音画延迟、主副字幕及被脚本删除的临时 ASS 均由播放快照恢复。

本地文件通过绑定 NetBird 地址的临时 Range 接口提供给服务端，只共享选定文件。网站使用 mpv 已解析的媒体 URL；B 站插件按实际选中轨道解析 DASH，并查询同一编码的官方备用地址。网页解析器是 mpv 原有环境的一部分。

## 安装与配置

Gentoo ebuild 位于 [gentoo-cs](https://github.com/csbxd/gentoo-cs)。

```sh
emerge --ask app-misc/modeld::gentoo-cs
emerge --ask media-plugins/mpv-modeld::gentoo-cs
```

服务端：使用 [server.example.json](config/server.example.json) 配置 `/etc/modeld/server.json`，把 `listen` 改为 NetBird 地址，启用 systemd 或 OpenRC 的 `modeld`。Portage 管理 SDK、模型权重、服务账号和服务定义；`/etc/modeld/api.token` 在首次安装时生成，升级保留。首次启动预热 1920×1080 模型，`GET /healthz` 返回 ready 后可用。

客户端：把 [client.example.json](config/client.example.json) 保存为 `~/.config/modeld/client.json`，配置 `endpoint` 和私有 `token_file`，文件权限设为 0600。`connect_address` 可指定实际 NetBird IP，保留 HTTP Host、TLS SNI 和证书校验使用的域名；`source_bind` 默认自动选择 NetBird 地址。

将 `/usr/lib64/mpv/scripts/modeld.so` 链接到 `~/.config/mpv/scripts/modeld.so`，重开 mpv。2.0 要求匹配的新客户端、服务端和配置结构；部署时一次性迁移配置，程序不包含旧协议适配。

## 管理

```sh
modelctl --version
modelctl check-config
modelctl health
modelctl models
modelctl status
modelctl sessions
modelctl inspect SESSION_ID
modelctl open request.json
modelctl lease SESSION_ID paused
modelctl close SESSION_ID
modelctl source video.mkv
modelctl logs 100
modelctl log-path
```

`open -` 从标准输入读取请求；`--config FILE` 选择配置。`source` 输出一个可放入请求的输入对象，命令退出即停止共享。`close` 对应 HTTP 204，因此打印 `null`。

日志默认在 `~/.local/state/modeld/mpv.log`，跟随 `XDG_STATE_HOME`，0600 权限并按 2 MiB 轮换。日志记录版本、播放阶段、请求状态、缓存、定位和 mpv 警告，并隐藏凭据、媒体路径和签名参数。服务端日志包含事件、源探测、渲染与队列故障，状态接口提供当前会话与队列信息。

## 画质与范围

当前模型为 RIFE 4.26 Heavy FP32，TensorRT-RTX 后端，禁用 TF32，CUDA Graph 双流处理。支持原生偶数尺寸、最长边不超过 1920、最短边不超过 1080、低于 60 fps 的可定位 SDR 媒体，固定输出 60 fps。AV1 输入使用 FFmpeg libdav1d。

默认传输为 8 位 HEVC NVENC、QP 18，视频编码有损；音轨保持原编码。服务端可选择 HEVC/H.264 和 QP。网络、编码与解码共同影响播放，输出格式为 60 fps 不代表任意片源都能端到端实时。HDR、4K、直播、DRM、多段拼接输入、超分与 LLM 尚未提供。

模型来自 [vs-mlrt external models](https://github.com/AmusementClub/vs-mlrt/releases/tag/external-models)，SHA-256：

```text
990ceae958e3da49a112125fa2ba3d1bcc361e97012e609492e37f33527d7955
```

源码 MIT；NVIDIA SDK 使用其自身许可，模型与 SDK 由独立 ebuild 获取。[现有 GPU 后端对比数据](benchmarks/tensorrt/results/2026-11-17/README.md) 保留为独立开发记录。

## 开发与验证

```sh
cargo test --locked -p modeld-protocol

cmake -S . -B build-client -G Ninja -DMODELD_SERVER=OFF
cmake --build build-client
ctest --test-dir build-client --output-on-failure

cmake -S . -B build-server -G Ninja -DMODELD_CLIENT=OFF \
  -DMODELD_SERVER=ON -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-server
MODELD_NATIVE_LIB_DIR="$PWD/build-server" cargo test --locked
MODELD_NATIVE_LIB_DIR="$PWD/build-server" cargo clippy --locked --all-targets -- -D warnings
MODELD_NATIVE_LIB_DIR="$PWD/build-server" cargo build --locked --release
```

服务端始终链接实际原生后端。协议 crate 可在无 GPU 的机器测试；服务端路由、取消和调度测试使用测试专用 renderer，不构成生产回退。

`tests/native-api.mjs` 连接独立的实际 GPU 测试 API，验证超时、取消、原生音视频、音频备用源与竖屏。`tests/main-media.mjs` 使用真实渲染后的 12 秒夹具，主动阻塞后续分段，检查 mpv 缓冲等待、时钟停住、用户暂停值、缓存定位、被删除的 ASS、播放列表和开关复用。Node.js 仅用于手工集成测试。用 `bash tests/create-fixtures.sh 新目录` 生成夹具；`native-api.mjs` 的 `probes` 阶段使用 3 秒取源预算，`media` 阶段使用正常的 30 秒预算。`modeld --check-config FILE` 可在加载 GPU 之前校验服务端配置和凭据。
