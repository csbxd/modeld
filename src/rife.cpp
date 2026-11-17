#include "modeld/rife.hpp"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <array>
#include <cstring>
#include <cuda_runtime.h>
#include <fstream>
#include <iostream>
#include <openssl/evp.h>
#include <sstream>
#include <stdexcept>
namespace {
void cu(cudaError_t code) {
    if (code != cudaSuccess)
        throw std::runtime_error(std::string("CUDA: ") + cudaGetErrorString(code));
}
struct Logger : nvinfer1::ILogger {
    void log(Severity severity, const char *msg) noexcept override {
        if (severity <= Severity::kWARNING)
            std::cerr << "[TensorRT-RTX] " << msg << '\n';
    }
};
std::vector<char> read(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string hash(const std::vector<char> &data) {
    unsigned char md[32];
    unsigned n = 0;
    EVP_Digest(data.data(), data.size(), md, &n, EVP_sha256(), nullptr);
    static const char *chars = "0123456789abcdef";
    std::string h;
    for (unsigned i = 0; i < n; i++) {
        h += chars[md[i] >> 4];
        h += chars[md[i] & 15];
    }
    return h;
}
} // namespace
extern "C" cudaError_t modeld_pack_input(const uint8_t *, const uint8_t *, float *, int, float,
                                         cudaStream_t);
extern "C" cudaError_t modeld_pack_nv12(const float *, uint8_t *, uint8_t *, int, int, int, int,
                                        cudaStream_t);
namespace modeld {
struct Rife::Impl {
    Logger logger;
    int w, h;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    struct Slot {
        std::unique_ptr<nvinfer1::IExecutionContext> context;
        cudaStream_t stream = nullptr;
        cudaGraph_t graph = nullptr;
        cudaGraphExec_t exec = nullptr;
        cudaEvent_t ready = nullptr;
        uint8_t *first = nullptr, *last = nullptr, *host_first = nullptr, *host_last = nullptr;
        float *input = nullptr, *output = nullptr;
        uint64_t cached_first = 0, cached_last = 0;
        ~Slot() {
            if (stream)
                cudaStreamSynchronize(stream);
            if (exec)
                cudaGraphExecDestroy(exec);
            if (graph)
                cudaGraphDestroy(graph);
            context.reset();
            cudaFree(first);
            cudaFree(last);
            cudaFree(input);
            cudaFree(output);
            cudaFreeHost(host_first);
            cudaFreeHost(host_last);
            if (ready)
                cudaEventDestroy(ready);
            if (stream)
                cudaStreamDestroy(stream);
        }
    };
    std::array<Slot, 2> slots;
    Impl(int width, int height) : w(width), h(height) {}
};
Rife::Rife(const std::filesystem::path &model, const std::filesystem::path &directory, int w, int h)
    : impl(std::make_unique<Impl>(w, h)) {
    if (w < 32 || h < 32 || w > 1920 || h > 1080 || w % 2 || h % 2)
        throw std::runtime_error("Supported native dimensions: even 32..1920 x 32..1080");
    cu(cudaSetDevice(0));
    auto bytes = read(model);
    const auto fingerprint = hash(bytes);
    if (fingerprint != "990ceae958e3da49a112125fa2ba3d1bcc361e97012e609492e37f33527d7955")
        throw std::runtime_error(
            "Configured weights do not match the pinned RIFE 4.26 Heavy FP32 model");
    int driver = 0;
    cu(cudaDriverGetVersion(&driver));
    cudaDeviceProp prop{};
    cu(cudaGetDeviceProperties(&prop, 0));
    std::filesystem::create_directories(directory);
    auto key = fingerprint + "-" + std::to_string(w) + "x" + std::to_string(h) +
               "-fp32-no-tf32-rtx171116-sm" + std::to_string(prop.major) +
               std::to_string(prop.minor) + "-driver" + std::to_string(driver);
    auto path = directory / (key + ".plan");
    std::vector<char> plan;
    if (std::filesystem::exists(path))
        plan = read(path);
    impl->runtime.reset(nvinfer1::createInferRuntime(impl->logger));
    if (!plan.empty())
        impl->engine.reset(impl->runtime->deserializeCudaEngine(plan.data(), plan.size()));
    if (!impl->engine) {
        std::cerr << "[modeld] Building RIFE Heavy FP32 " << w << 'x' << h << " (TF32 disabled)\n";
        std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(impl->logger));
        std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
        std::unique_ptr<nvonnxparser::IParser> parser(
            nvonnxparser::createParser(*network, impl->logger));
        if (!parser->parse(bytes.data(), bytes.size()))
            throw std::runtime_error("Cannot parse RIFE ONNX model");
        if (network->getNbInputs() != 1 ||
            network->getInput(0)->getType() != nvinfer1::DataType::kFLOAT)
            throw std::runtime_error("A single packed FP32 input ONNX is required");
        auto shape = network->getInput(0)->getDimensions();
        if (shape.nbDims != 4)
            throw std::runtime_error("Invalid RIFE input shape");
        shape.d[0] = 1;
        shape.d[1] = 7;
        shape.d[2] = h;
        shape.d[3] = w;
        network->getInput(0)->setDimensions(shape);
        std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        config->clearFlag(nvinfer1::BuilderFlag::kTF32);
        config->setMaxAuxStreams(0);
        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 4ULL << 30);
        std::unique_ptr<nvinfer1::IHostMemory> memory(
            builder->buildSerializedNetwork(*network, *config));
        if (!memory)
            throw std::runtime_error("RIFE engine build failed");
        plan.assign(static_cast<const char *>(memory->data()),
                    static_cast<const char *>(memory->data()) + memory->size());
        impl->engine.reset(impl->runtime->deserializeCudaEngine(plan.data(), plan.size()));
        if (!impl->engine)
            throw std::runtime_error("RIFE engine load failed");
        auto tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(plan.data(), plan.size());
            if (!out)
                throw std::runtime_error("Cannot save compiled engine");
        }
        std::filesystem::rename(tmp, path);
    }
    std::string input, output;
    for (int i = 0; i < impl->engine->getNbIOTensors(); i++) {
        auto name = impl->engine->getIOTensorName(i);
        if (impl->engine->getTensorDataType(name) != nvinfer1::DataType::kFLOAT)
            throw std::runtime_error("Non-FP32 RIFE tensor");
        if (impl->engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
            input = name;
        else
            output = name;
    }
    auto shape = impl->engine->getTensorShape(output.c_str());
    if (shape.nbDims != 4 || shape.d[0] != 1 || shape.d[1] != 3 || shape.d[2] != h ||
        shape.d[3] != w)
        throw std::runtime_error("Unexpected RIFE output dimensions");
    const size_t pixels = static_cast<size_t>(w) * h;
    for (auto &s : impl->slots) {
        cu(cudaStreamCreateWithFlags(&s.stream, cudaStreamNonBlocking));
        cu(cudaEventCreateWithFlags(&s.ready, cudaEventDisableTiming));
        cu(cudaMalloc(&s.first, pixels * 3));
        cu(cudaMalloc(&s.last, pixels * 3));
        cu(cudaMallocHost(&s.host_first, pixels * 3));
        cu(cudaMallocHost(&s.host_last, pixels * 3));
        cu(cudaMalloc(&s.input, pixels * 7 * sizeof(float)));
        cu(cudaMalloc(&s.output, pixels * 3 * sizeof(float)));
        cu(cudaMemsetAsync(s.input, 0, pixels * 7 * sizeof(float), s.stream));
        s.context.reset(impl->engine->createExecutionContext());
        if (!s.context)
            throw std::runtime_error("Cannot create RIFE execution context");
        s.context->setTensorAddress(input.c_str(), s.input);
        s.context->setTensorAddress(output.c_str(), s.output);
        for (int i = 0; i < 4; i++)
            if (!s.context->enqueueV3(s.stream))
                throw std::runtime_error("RIFE warmup failed");
        cu(cudaStreamSynchronize(s.stream));
        cu(cudaStreamBeginCapture(s.stream, cudaStreamCaptureModeGlobal));
        if (!s.context->enqueueV3(s.stream))
            throw std::runtime_error("RIFE graph capture failed");
        cu(cudaStreamEndCapture(s.stream, &s.graph));
        cu(cudaGraphInstantiateWithFlags(&s.exec, s.graph, 0));
    }
    std::cerr << "[modeld] Ready: RIFE 4.26 Heavy FP32, " << w << 'x' << h
              << ", CUDA Graph, two streams\n";
}
Rife::~Rife() = default;
int Rife::width() const {
    return impl->w;
}
int Rife::height() const {
    return impl->h;
}
void Rife::enqueue(int slot, const Image &a, const Image &b, float t, uint8_t *y, uint8_t *uv,
                   int yp, int uvp) {
    cu(cudaSetDevice(0));
    if (a.width != impl->w || a.height != impl->h || b.width != impl->w || b.height != impl->h ||
        a.rgb.size() != static_cast<size_t>(impl->w) * impl->h * 3 || a.rgb.size() != b.rgb.size())
        throw std::runtime_error("RIFE image shape mismatch");
    auto &s = impl->slots.at(slot);
    const size_t size = a.rgb.size();
    cu(cudaStreamSynchronize(s.stream));
    if (s.cached_first != a.id) {
        std::memcpy(s.host_first, a.rgb.data(), size);
        cu(cudaMemcpyAsync(s.first, s.host_first, size, cudaMemcpyHostToDevice, s.stream));
        s.cached_first = a.id;
    }
    if (s.cached_last != b.id) {
        std::memcpy(s.host_last, b.rgb.data(), size);
        cu(cudaMemcpyAsync(s.last, s.host_last, size, cudaMemcpyHostToDevice, s.stream));
        s.cached_last = b.id;
    }
    cu(modeld_pack_input(s.first, s.last, s.input, impl->w * impl->h, t, s.stream));
    const float *rgb = s.input;
    if (t > 1e-6f && t < 1.0f - 1e-6f) {
        cu(cudaGraphLaunch(s.exec, s.stream));
        rgb = s.output;
    } else if (t >= 1.0f - 1e-6f)
        rgb = s.input + 3 * impl->w * impl->h;
    cu(modeld_pack_nv12(rgb, y, uv, impl->w, impl->h, yp, uvp, s.stream));
    cu(cudaEventRecord(s.ready, s.stream));
}
void Rife::wait(int slot) {
    cu(cudaEventSynchronize(impl->slots.at(slot).ready));
}
} // namespace modeld
