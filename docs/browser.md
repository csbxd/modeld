# B 站浏览器插帧

Chrome Manifest V3 扩展，在 B 站原播放器里使用远端 RIFE 4.26 Heavy FP32。原播放按钮、进度条、音量、倍速、全屏和网页弹幕继续使用同一个 video 元素。普通窗口下，点击播放器下方“插帧 60 FPS”；全屏和网页全屏下，按钮位于原播放器控制条。也可按 **Alt+Shift+I**；再次点击恢复 B 站原流。

## 配置

扩展设置页提供服务地址、可选直连 IP、API 密钥、前后缓冲、分段缓存、编码参数和自动开启选项。默认读取本机 `~/.config/modeld/client.json`，无需再次填写当前服务器信息。用户更改的浏览器连接配置保存在 `~/.config/modeld/browser.json`，新输入密钥写入该文件旁的私有 token 文件，权限均为 0600。

密钥由本机 native messaging 程序读取，不放入网页或扩展消息中的媒体地址。播放偏好保存在 Chrome 本机存储。默认手动开启，二维日漫保持关闭；自动开启需自行确认片源类型。

支持当前 modeld 的 SDR、原生 1080p 范围、低于 60 fps 的输入。扩展默认选择 AV1 VBR，输出 60 fps 分片 MP4 与原 AAC-LC 音轨。目标码率和峰值码率各自可填 1 至 20 Mbps 的整数，默认 12 和 18 Mbps。目标须小于或等于峰值。2.1.6 保存的峰值会沿用，首次显示的目标按原联动值取整。独立码率设置需要 modeld 2.1.7 或更新版本、支持 AV1 编码的 NVENC 显卡和支持 AV1 MediaSource 播放的浏览器。设置页也可选择 H.264，默认 QP 18。保存后重新开启插帧即可使用所选编码。

开启前检查实际选中的音轨。当前浏览器传输支持标准 AAC-LC；选中 Dolby 或 Hi-Res 音轨时，页面会提示切换音质，由用户选择后再开启插帧。


前向缓冲可填写至少 4 秒的安全整数，后向缓冲可填写至少 0 秒的安全整数，例如 3600 或 86400。设置页和配置校验均接受这些大数值。实际缓存范围受浏览器媒体内存配额影响，配额不足时等待后续重试，已回收的分段可重新读取。保存后在下次开启插帧时使用新设置。

## Gentoo 安装

```sh
emerge --ask app-misc/modeld::gentoo-cs
emerge --ask www-plugins/modeld-browser::gentoo-cs
```

需要 modeld 2.1 或更新版本。浏览器包安装本机连接程序、Chrome native host 注册和 Linux Chrome 安装策略。扩展通过签名 CRX 更新，Chrome 中可以停用。新安装或升级后刷新 B 站页面；浏览器若尚未读取策略，重开 Chrome。

扩展 ID：`iegbohgbehnfnnnkmgaolhhffinebpdk`。本机组件名称：`org.csbxd.modeld.browser`。无需 mpv、Python 或 VapourSynth。

## 实现

- 扩展从当前 B 站播放器读取实际选中的视频、AAC 音轨和官方备用 URL，保留完整文件名及签名。
- Chrome service worker 通过 native messaging 与 C++ 本机组件通信；因此 HTTPS 网页不需要直接连接 HTTP NetBird 服务。
- 本机组件只接收受限动作及 B 站媒体主机，API 密钥在本机读取。二进制响应拆分为小于 1 MiB 的消息，避免 Chrome native messaging 大小限制。
- `POST /api/v2/browser/sessions` 通过 `video_codec` 选择浏览器的 H.264 或 AV1 分片 MP4 输出。
- fMP4 初始化头共享，片段保留绝对音视频解码时间，独立片段可用于远距离定位。
- 播放时暂停并卸下原 DASH source，以 MediaSource 接管相同 video。播放器内核通过 adapter 读取新的时钟和缓冲；关闭时重接原 DASH source 并恢复当前位置。原播放器可继续自行选择合适的源画质。
- 关闭时并行恢复原播放并回收远端会话，关闭操作完成后可再次开启插帧。
- 分段按需读取，MSE 保留配置的后向范围，后台字节缓存按 LRU 回收。页面离开或会话关闭会取消远端工作。
- 末段追加完成后调用 MediaSource.endOfStream，让浏览器正常触发片尾事件。片尾后定位到未缓冲的位置仍可追加分段并继续播放。
- 普通窗口控件由 document.body 下的独立节点承载，播放器底部预留控件空间；定位随滚动和尺寸变化更新。页面框架继续管理播放器周围的节点。全屏下将控件放入播放器原生控制条。

## 开发验证

```sh
cmake -S . -B build-browser -G Ninja -DMODELD_CLIENT=OFF \
  -DMODELD_SERVER=OFF -DMODELD_BROWSER=ON
cmake --build build-browser
ctest --test-dir build-browser --output-on-failure
node --test tests/browser*.test.mjs
```

Chrome 开发测试使用独立 profile、开发者模式和 unpacked 扩展。`tools/browser/verify.mjs` 通过实际 Chrome 检查 B 站播放、解码帧数、原控制条暂停/定位及恢复。正式安装使用已签名 CRX，不要求用户开启开发者模式。

对已打开的测试视频运行 `node tools/browser/verify-end.mjs CHROME_PROFILE REPORT_DIRECTORY BVID`，可验证片尾结束、结束后向未缓冲位置定位、普通窗口控件布局和全屏切换。对应回归记录在 `docs/validation-2.1.1-browser.json`。

在独立测试 profile 中运行 `node tools/browser/verify-page.mjs CHROME_PROFILE REPORT_DIRECTORY BVID`，会新建测试页，检查页面初始化、开启插帧后延迟加载评论、两次开关后的节点身份和顺序、播放器外 DOM 变更、音量和倍速保留，以及网页全屏切换。测试 GPU 会话使用该 profile 注册的 native host 配置。对应记录在 `docs/validation-2.1.2-browser.json`。

签名私钥保存在维护者的私有目录，不随源码或 ebuild 分发。扩展公钥在 manifest 中固定，便于本机程序限制允许的扩展来源。
