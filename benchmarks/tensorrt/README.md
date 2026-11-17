# TensorRT / TensorRT-RTX 原生对比

同一份 C++ 测试代码分别链接两套 SDK，复用 modeld 的 CUDA RGB 输入打包和 NV12 输出内核。测试工具与服务实现均不使用 Python。

模型固定为 RIFE 4.26 Heavy packed FP32，SHA-256 为 `990ceae958e3da49a112125fa2ba3d1bcc361e97012e609492e37f33527d7955`。统一关闭 TF32，设置 `NVIDIA_TF32_OVERRIDE=0`、4 GiB 工作区、0 条辅助流、两个执行上下文。各上下文使用独立 CUDA Graph。

## 数据与指标

`prepare.sh` 从 [Xiph 测试集](https://media.xiph.org/video/derf/) 按字节范围提取原生分辨率三帧：1080p 的 Ducks Take Off、Crowd Run、Park Joy，以及 720p 的 Four People、Parkrun。每段取第 30–32 和 120–122 帧，共 10 组三帧。转换为 BT.709 limited YUV → full RGB24，保持原始尺寸。转换后的字节流校验和随结果保留，两个后端读取相同文件。

每个样本取前后帧，分别生成 t=0.25、0.5、0.75 的结果，共比较 30 张预测帧。t=0.5 还与真实中间帧比较。导出的是编码前的 NCHW FP32 RGB；PSNR 峰值取 1，SSIM 使用 BT.709 全范围亮度、11×11 高斯窗口、sigma=1.5 和总体协方差，排除五像素边界。8 位差异按裁剪到 [0,1] 后就近取整的 RGB 通道统计。

测速分三项：单流 CUDA Graph 的 GPU event 延迟、双流 Graph 吞吐、双流完整 GPU 管线吞吐。最后一项包含两张 RGB 图片上传、输入预处理、推理和 NV12 转换，上传量比生产中复用相邻源帧的做法更保守。三项均不包含源解码、NVENC、音视频同步和网络传输。

每个分辨率先生成新测试 plan，记录构建及上下文准备时间；随后使用缓存 plan 按 RTX → TRT → TRT → RTX 顺序重启进程。每次至少预热三秒，再做三轮计时；每轮单流 100 次、双流 200 次、完整 GPU 管线 200 次。报告汇总两次缓存进程、六轮计时的中位数和范围。测速使用每个尺寸第一组自然帧，t 固定为 0.5。

额外对普通 TensorRT 的 1080p 引擎设置构建优化等级 5，保持其他资源限制相同，重新构建并重复缓存进程测试；默认基线使用 SDK 默认等级 3。

## 重现

在 NVIDIA 主机分别安装 TensorRT-RTX 和普通 TensorRT C++ SDK。普通版以 `/opt/tensorrt` 独立安装，测试进程设置其库路径，不修改正式服务的加载路径。

新一轮构建计时须使用新的运行目录；脚本发现已有 plan 会退出并保留文件，避免把缓存加载误记为首次构建。离线汇总脚本需要 Node.js 20 或更新版本，GPU 测试本身由 C++/CUDA 执行。

```sh
bash benchmarks/tensorrt/prepare.sh "$HOME/.cache/modeld/trt-compare/samples"

cmake -S benchmarks/tensorrt -B build-bench-rtx -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DBACKEND=rtx -DSDK_ROOT=/opt/tensorrt-rtx
cmake --build build-bench-rtx

cmake -S benchmarks/tensorrt -B build-bench-trt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=89 \
  -DBACKEND=trt -DSDK_ROOT=/opt/tensorrt
cmake --build build-bench-trt

build-bench-trt/compare-output --self-test
bash benchmarks/tensorrt/run.sh "$HOME/.cache/modeld/trt-compare" "$PWD"
```

优化等级 5 的补充测试可设置 `MODELD_BENCH_OPT_LEVEL=5` 再调用 `trt-bench`。其 plan 文件名包含等级，避免复用默认引擎。

FPS 的单位是“生成的新插值帧/秒”。例如精确的 30→60 fps 需要每秒生成 30 张新帧；实际播放还需要为解码、原始帧处理、编码与传输预留时间。跨后端 PSNR/SSIM 用来量化数值一致性，真实中间帧指标用于观察模型重建误差，二者含义不同。
