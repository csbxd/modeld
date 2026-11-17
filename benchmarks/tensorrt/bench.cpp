#include <NvInfer.h>
#include <NvInferVersion.h>
#include <NvOnnxParser.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cuda_runtime.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <openssl/evp.h>
#include <stdexcept>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
extern "C" cudaError_t modeld_pack_input(const uint8_t *, const uint8_t *, float *, int, float,
                                         cudaStream_t);
extern "C" cudaError_t modeld_pack_nv12(const float *, uint8_t *, uint8_t *, int, int, int, int,
                                        cudaStream_t);
namespace {
void cu(cudaError_t value) {
    if (value != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(value));
}
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Logger : nvinfer1::ILogger {
    void log(Severity s, const char *message) noexcept override {
        if (s <= Severity::kWARNING)
            std::cerr << '[' << BENCH_BACKEND << "] " << message << '\n';
    }
} logger;
std::vector<char> read(const std::filesystem::path &file) {
    std::ifstream input(file, std::ios::binary);
    require(bool(input), ("Cannot read " + file.string()).c_str());
    return {std::istreambuf_iterator<char>(input), {}};
}
double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
std::string digest(const std::vector<char> &bytes) {
    unsigned char md[32];
    unsigned count = 0;
    EVP_Digest(bytes.data(), bytes.size(), md, &count, EVP_sha256(), nullptr);
    std::string text;
    for (unsigned i = 0; i < count; ++i) {
        text += "0123456789abcdef"[md[i] >> 4];
        text += "0123456789abcdef"[md[i] & 15];
    }
    return text;
}
Json stats(std::vector<double> values) {
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    return {{"mean_ms", mean},
            {"median_ms", sorted[sorted.size() / 2]},
            {"p95_ms", sorted[static_cast<size_t>(std::ceil(sorted.size() * 0.95)) - 1]},
            {"fps", 1000.0 / mean},
            {"samples_ms", values}};
}
struct Slot {
    std::unique_ptr<nvinfer1::IExecutionContext> context;
    cudaStream_t stream = nullptr;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t executable = nullptr;
    uint8_t *first = nullptr, *last = nullptr, *yuv = nullptr, *host_first = nullptr,
            *host_last = nullptr;
    float *input = nullptr, *output = nullptr;
    ~Slot() {
        if (stream)
            cudaStreamSynchronize(stream);
        if (executable)
            cudaGraphExecDestroy(executable);
        if (graph)
            cudaGraphDestroy(graph);
        context.reset();
        cudaFree(first);
        cudaFree(last);
        cudaFree(yuv);
        cudaFree(input);
        cudaFree(output);
        cudaFreeHost(host_first);
        cudaFreeHost(host_last);
        if (stream)
            cudaStreamDestroy(stream);
    }
    void upload(int pixels, float t) {
        cu(cudaMemcpyAsync(first, host_first, pixels * 3, cudaMemcpyHostToDevice, stream));
        cu(cudaMemcpyAsync(last, host_last, pixels * 3, cudaMemcpyHostToDevice, stream));
        cu(modeld_pack_input(first, last, input, pixels, t, stream));
    }
};
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 9)
            throw std::runtime_error("Usage: trt-bench MODEL CACHE WIDTH HEIGHT MANIFEST OUTDIR "
                                     "LABEL --export|--no-export");
        const auto start = Clock::now();
        const std::filesystem::path model = argv[1], cache = argv[2], manifest = argv[5],
                                    output_dir = argv[6];
        const int w = std::stoi(argv[3]), h = std::stoi(argv[4]), pixels = w * h;
        const std::string label = argv[7];
        const bool export_output = std::string(argv[8]) == "--export";
        require(export_output || std::string(argv[8]) == "--no-export",
                "Expected --export or --no-export");
        require(w > 0 && h > 0 && w % 2 == 0 && h % 2 == 0, "Invalid dimensions");
        std::filesystem::create_directories(cache);
        std::filesystem::create_directories(output_dir);
        auto document = Json::parse(read(manifest));
        Json selected = Json::array();
        for (const auto &sample : document.at("samples"))
            if (sample.at("width") == w && sample.at("height") == h)
                selected.push_back(sample);
        require(!selected.empty(), "No native-resolution samples for this shape");
        auto sample_bytes =
            read(manifest.parent_path() / selected[0].at("rgb_file").get<std::string>());
        require(sample_bytes.size() == static_cast<size_t>(pixels) * 9,
                "Triplet RGB length mismatch");
        const auto model_bytes = read(model);
        const auto sha = digest(model_bytes);
        require(sha == "990ceae958e3da49a112125fa2ba3d1bcc361e97012e609492e37f33527d7955",
                "Wrong model weights");
        cu(cudaSetDevice(0));
        cudaDeviceProp device{};
        cu(cudaGetDeviceProperties(&device, 0));
        int driver = 0, runtime_version = 0;
        cu(cudaDriverGetVersion(&driver));
        cu(cudaRuntimeGetVersion(&runtime_version));
        size_t free_before = 0, total = 0;
        cu(cudaMemGetInfo(&free_before, &total));
        const std::string version =
            std::to_string(NV_TENSORRT_MAJOR) + "." + std::to_string(NV_TENSORRT_MINOR) + "." +
            std::to_string(NV_TENSORRT_PATCH) + "." + std::to_string(NV_TENSORRT_BUILD);
        auto plan_file =
            cache / (std::string(BENCH_BACKEND) + "-" + version + "-" + std::to_string(w) + "x" +
                     std::to_string(h) + "-fp32-no-tf32-driver" + std::to_string(driver) + ".plan");
        auto load_start = Clock::now();
        std::unique_ptr<nvinfer1::IRuntime> runtime(nvinfer1::createInferRuntime(logger));
        require(bool(runtime), "Cannot create inference runtime");
        std::vector<char> plan;
        bool cache_hit = std::filesystem::exists(plan_file);
        double build_seconds = 0;
        if (cache_hit)
            plan = read(plan_file);
        if (plan.empty()) {
            auto build_start = Clock::now();
            std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));
            require(bool(builder), "Cannot create builder");
            std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
            std::unique_ptr<nvonnxparser::IParser> parser(
                nvonnxparser::createParser(*network, logger));
            require(parser->parse(model_bytes.data(), model_bytes.size()), "ONNX parse failed");
            require(network->getNbInputs() == 1, "Expected packed input");
            auto dimensions = network->getInput(0)->getDimensions();
            require(dimensions.nbDims == 4, "Expected NCHW input");
            dimensions.d[0] = 1;
            dimensions.d[1] = 7;
            dimensions.d[2] = h;
            dimensions.d[3] = w;
            network->getInput(0)->setDimensions(dimensions);
            std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
            config->clearFlag(nvinfer1::BuilderFlag::kTF32);
            config->setMaxAuxStreams(0);
            config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 4ULL << 30);
            std::unique_ptr<nvinfer1::IHostMemory> compiled(
                builder->buildSerializedNetwork(*network, *config));
            require(bool(compiled), "Engine build failed");
            plan.assign(static_cast<const char *>(compiled->data()),
                        static_cast<const char *>(compiled->data()) + compiled->size());
            std::ofstream file(plan_file, std::ios::binary);
            file.write(plan.data(), plan.size());
            require(bool(file), "Cannot save engine");
            build_seconds = seconds(build_start);
        }
        std::unique_ptr<nvinfer1::ICudaEngine> engine(
            runtime->deserializeCudaEngine(plan.data(), plan.size()));
        require(bool(engine), "Cannot load engine");
        const auto load_seconds = seconds(load_start);
        std::string input_name, output_name;
        require(engine->getNbIOTensors() == 2, "Expected one input and output");
        for (int i = 0; i < engine->getNbIOTensors(); ++i) {
            const auto *name = engine->getIOTensorName(i);
            require(engine->getTensorDataType(name) == nvinfer1::DataType::kFLOAT, "Non-FP32 I/O");
            if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
                input_name = name;
            else
                output_name = name;
        }
        auto shape = engine->getTensorShape(output_name.c_str());
        require(shape.nbDims == 4 && shape.d[0] == 1 && shape.d[1] == 3 && shape.d[2] == h &&
                    shape.d[3] == w,
                "Output shape mismatch");
        std::array<Slot, 2> slots;
        auto prepare_start = Clock::now();
        for (auto &slot : slots) {
            cu(cudaStreamCreateWithFlags(&slot.stream, cudaStreamNonBlocking));
            cu(cudaMallocHost(&slot.host_first, static_cast<size_t>(pixels) * 3));
            cu(cudaMallocHost(&slot.host_last, static_cast<size_t>(pixels) * 3));
            cu(cudaMalloc(&slot.first, static_cast<size_t>(pixels) * 3));
            cu(cudaMalloc(&slot.last, static_cast<size_t>(pixels) * 3));
            cu(cudaMalloc(&slot.yuv, static_cast<size_t>(pixels) * 3 / 2));
            cu(cudaMalloc(&slot.input, static_cast<size_t>(pixels) * 7 * sizeof(float)));
            cu(cudaMalloc(&slot.output, static_cast<size_t>(pixels) * 3 * sizeof(float)));
            std::copy_n(sample_bytes.data(), pixels * 3, reinterpret_cast<char *>(slot.host_first));
            std::copy_n(sample_bytes.data() + pixels * 6, pixels * 3,
                        reinterpret_cast<char *>(slot.host_last));
            slot.context.reset(engine->createExecutionContext());
            require(bool(slot.context), "Cannot create execution context");
            require(slot.context->setTensorAddress(input_name.c_str(), slot.input),
                    "Input binding failed");
            require(slot.context->setTensorAddress(output_name.c_str(), slot.output),
                    "Output binding failed");
            slot.upload(pixels, 0.5f);
            for (int i = 0; i < 20; ++i)
                require(slot.context->enqueueV3(slot.stream), "Warmup enqueue failed");
            cu(cudaStreamSynchronize(slot.stream));
            cu(cudaStreamBeginCapture(slot.stream, cudaStreamCaptureModeGlobal));
            require(slot.context->enqueueV3(slot.stream), "Capture enqueue failed");
            cu(cudaStreamEndCapture(slot.stream, &slot.graph));
            cu(cudaGraphInstantiateWithFlags(&slot.executable, slot.graph, 0));
        }
        const double prepare_seconds = seconds(prepare_start);
        auto warm_start = Clock::now();
        do {
            for (int i = 0; i < 16; ++i)
                cu(cudaGraphLaunch(slots[i % 2].executable, slots[i % 2].stream));
            cu(cudaDeviceSynchronize());
        } while (seconds(warm_start) < 3.0);
        size_t free_after = 0;
        cu(cudaMemGetInfo(&free_after, &total));
        Json trials = Json::array();
        for (int trial = 0; trial < 3; ++trial) {
            constexpr int iterations = 100;
            std::array<cudaEvent_t, iterations> begin{}, end{};
            for (int i = 0; i < iterations; ++i) {
                cu(cudaEventCreate(&begin[i]));
                cu(cudaEventCreate(&end[i]));
                cu(cudaEventRecord(begin[i], slots[0].stream));
                cu(cudaGraphLaunch(slots[0].executable, slots[0].stream));
                cu(cudaEventRecord(end[i], slots[0].stream));
            }
            cu(cudaStreamSynchronize(slots[0].stream));
            std::vector<double> latencies;
            for (int i = 0; i < iterations; ++i) {
                float ms = 0;
                cu(cudaEventElapsedTime(&ms, begin[i], end[i]));
                latencies.push_back(ms);
                cu(cudaEventDestroy(begin[i]));
                cu(cudaEventDestroy(end[i]));
            }
            auto dual_start = Clock::now();
            for (int i = 0; i < iterations * 2; ++i)
                cu(cudaGraphLaunch(slots[i % 2].executable, slots[i % 2].stream));
            cu(cudaDeviceSynchronize());
            const double dual_seconds = seconds(dual_start);
            auto pipeline_start = Clock::now();
            for (int i = 0; i < iterations * 2; ++i) {
                auto &slot = slots[i % 2];
                slot.upload(pixels, 0.5f);
                cu(cudaGraphLaunch(slot.executable, slot.stream));
                cu(modeld_pack_nv12(slot.output, slot.yuv, slot.yuv + pixels, w, h, w, w,
                                    slot.stream));
            }
            cu(cudaDeviceSynchronize());
            trials.push_back(
                {{"single_stream", stats(latencies)},
                 {"dual_stream_fps", iterations * 2 / dual_seconds},
                 {"uploaded_rgb_to_nv12_fps", iterations * 2 / seconds(pipeline_start)}});
        }
        if (export_output) {
            for (const auto &sample : selected) {
                const auto raw =
                    read(manifest.parent_path() / sample.at("rgb_file").get<std::string>());
                require(raw.size() == static_cast<size_t>(pixels) * 9,
                        "Sample byte count mismatch");
                auto &slot = slots[0];
                std::copy_n(raw.data(), pixels * 3, reinterpret_cast<char *>(slot.host_first));
                std::copy_n(raw.data() + pixels * 6, pixels * 3,
                            reinterpret_cast<char *>(slot.host_last));
                for (const auto t : {0.25f, 0.5f, 0.75f}) {
                    slot.upload(pixels, t);
                    cu(cudaGraphLaunch(slot.executable, slot.stream));
                    std::vector<float> rgb(static_cast<size_t>(pixels) * 3);
                    cu(cudaMemcpyAsync(rgb.data(), slot.output, rgb.size() * sizeof(float),
                                       cudaMemcpyDeviceToHost, slot.stream));
                    cu(cudaStreamSynchronize(slot.stream));
                    require(std::all_of(rgb.begin(), rgb.end(),
                                        [](float x) { return std::isfinite(x); }),
                            "Non-finite model output");
                    const auto file =
                        output_dir /
                        (std::string(BENCH_BACKEND) + "-" + sample.at("id").get<std::string>() +
                         "-t" + std::to_string(static_cast<int>(t * 100)) + ".f32");
                    std::ofstream out(file, std::ios::binary);
                    out.write(reinterpret_cast<const char *>(rgb.data()),
                              rgb.size() * sizeof(float));
                    require(bool(out), "Output export failed");
                }
            }
        }
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        Json result = {{"backend", BENCH_BACKEND},
                       {"sdk_version", version},
                       {"model_sha256", sha},
                       {"width", w},
                       {"height", h},
                       {"device", device.name},
                       {"cuda_driver", driver},
                       {"cuda_runtime", runtime_version},
                       {"tf32", false},
                       {"precision", "fp32"},
                       {"workspace_bytes", 4ULL << 30},
                       {"aux_streams", 0},
                       {"execution_contexts", 2},
                       {"cache_hit", cache_hit},
                       {"engine_bytes", plan.size()},
                       {"build_seconds", build_seconds},
                       {"load_including_build_seconds", load_seconds},
                       {"context_warmup_and_graph_capture_seconds", prepare_seconds},
                       {"gpu_allocation_delta_bytes",
                        static_cast<int64_t>(free_before) - static_cast<int64_t>(free_after)},
                       {"gpu_free_before_bytes", free_before},
                       {"gpu_free_after_bytes", free_after},
                       {"pid", getpid()},
                       {"cuda_graph", true},
                       {"nvidia_tf32_override", std::getenv("NVIDIA_TF32_OVERRIDE")
                                                    ? std::getenv("NVIDIA_TF32_OVERRIDE")
                                                    : "unset"},
                       {"process_peak_rss_kib", usage.ru_maxrss},
                       {"label", label},
                       {"trials", trials},
                       {"wall_seconds", seconds(start)}};
        auto result_file = output_dir / (label + ".json");
        std::ofstream out(result_file);
        out << result.dump(2) << '\n';
        std::cout << Json{{"backend", BENCH_BACKEND},
                          {"shape", std::to_string(w) + "x" + std::to_string(h)},
                          {"build_seconds", build_seconds},
                          {"median_ms", trials[1]["single_stream"]["median_ms"]},
                          {"dual_stream_fps", trials[1]["dual_stream_fps"]},
                          {"gpu_pipeline_fps", trials[1]["uploaded_rgb_to_nv12_fps"]}}
                         .dump()
                  << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
