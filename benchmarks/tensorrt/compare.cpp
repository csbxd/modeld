#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <numeric>
#include <stdexcept>
#include <vector>

using Json = nlohmann::json;
namespace {
template <typename T> std::vector<T> read(const std::filesystem::path &file, size_t count) {
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != static_cast<std::streamoff>(count * sizeof(T)))
        throw std::runtime_error("Bad file: " + file.string());
    input.seekg(0);
    std::vector<T> result(count);
    input.read(reinterpret_cast<char *>(result.data()), count * sizeof(T));
    return result;
}
Json psnr(double mse) {
    return mse > 0 ? Json(-10 * std::log10(mse)) : Json("infinity");
}
uint8_t byte(float x) {
    return static_cast<uint8_t>(
        std::clamp(std::nearbyint(std::clamp(x, 0.0f, 1.0f) * 255.0f), 0.0f, 255.0f));
}
std::vector<double> luma(const std::vector<float> &rgb, size_t n) {
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = .2126 * std::clamp(rgb[i], 0.0f, 1.0f) +
                 .7152 * std::clamp(rgb[n + i], 0.0f, 1.0f) +
                 .0722 * std::clamp(rgb[2 * n + i], 0.0f, 1.0f);
    return out;
}
// Wang-style SSIM: normalized luma, 11x11 Gaussian window sigma=1.5,
// population covariance, C1=.01^2 C2=.03^2; discard five boundary pixels.
double ssim(const std::vector<double> &a, const std::vector<double> &b, int w, int h) {
    using Moments = std::array<double, 5>;
    std::array<double, 11> kernel{};
    double sum = 0;
    for (int i = -5; i <= 5; ++i) {
        kernel[i + 5] = std::exp(-i * i / 4.5);
        sum += kernel[i + 5];
    }
    for (auto &x : kernel)
        x /= sum;
    std::vector<Moments> horizontal(static_cast<size_t>(w) * h);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y)
        for (int x = 5; x < w - 5; ++x) {
            Moments m{};
            for (int k = -5; k <= 5; ++k) {
                const size_t i = static_cast<size_t>(y) * w + x + k;
                double p = a[i], q = b[i], c = kernel[k + 5];
                m[0] += c * p;
                m[1] += c * q;
                m[2] += c * p * p;
                m[3] += c * q * q;
                m[4] += c * p * q;
            }
            horizontal[static_cast<size_t>(y) * w + x] = m;
        }
    double total = 0;
#pragma omp parallel for reduction(+ : total) schedule(static)
    for (int y = 5; y < h - 5; ++y)
        for (int x = 5; x < w - 5; ++x) {
            Moments m{};
            for (int k = -5; k <= 5; ++k)
                for (int j = 0; j < 5; ++j)
                    m[j] += kernel[k + 5] * horizontal[static_cast<size_t>(y + k) * w + x][j];
            double va = std::max(0.0, m[2] - m[0] * m[0]), vb = std::max(0.0, m[3] - m[1] * m[1]),
                   cov = m[4] - m[0] * m[1];
            total += ((2 * m[0] * m[1] + .0001) * (2 * cov + .0009)) /
                     ((m[0] * m[0] + m[1] * m[1] + .0001) * (va + vb + .0009));
        }
    return total / static_cast<double>((w - 10) * (h - 10));
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            constexpr int width = 32, height = 32;
            std::vector<double> a(width * height, 0.25), b(width * height, 0.75);
            const double expected = (2 * .25 * .75 + .0001) / (.25 * .25 + .75 * .75 + .0001);
            if (std::abs(ssim(a, a, width, height) - 1) > 1e-10 ||
                std::abs(ssim(a, b, width, height) - expected) > 1e-10 ||
                std::abs(psnr(.0001).get<double>() - 40) > 1e-12 || byte(.5f) != 128)
                throw std::runtime_error("Analytical SSIM/PSNR/quantization test failed");
            std::cout << "Analytical SSIM, PSNR and 8-bit quantization checks passed\n";
            return 0;
        }
        if (argc != 5)
            throw std::runtime_error("Usage: compare-output MANIFEST OUTPUTS REPORT SUMMARY");
        std::ifstream file(argv[1]);
        Json manifest;
        file >> manifest;
        const auto root = std::filesystem::path(argv[1]).parent_path(),
                   out = std::filesystem::path(argv[2]);
        Json rows = Json::array();
        for (const auto &sample : manifest.at("samples")) {
            const int w = sample.at("width"), h = sample.at("height");
            const size_t n = static_cast<size_t>(w) * h;
            const auto raw = read<uint8_t>(root / sample.at("rgb_file").get<std::string>(), n * 9);
            std::vector<float> truth(n * 3);
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < 3; ++c)
                    truth[c * n + i] = raw[3 * n + 3 * i + c] / 255.0f;
            const auto truth_y = luma(truth, n);
            for (int t : {25, 50, 75}) {
                const auto suffix =
                    sample.at("id").get<std::string>() + "-t" + std::to_string(t) + ".f32";
                const auto a = read<float>(out / ("rtx-" + suffix), n * 3),
                           b = read<float>(out / ("trt-" + suffix), n * 3);
                double abs_sum = 0, squares = 0, maximum = 0;
                size_t changed8 = 0;
                int max8 = 0;
                double gt_a = 0, gt_b = 0;
                for (size_t i = 0; i < a.size(); ++i) {
                    if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
                        throw std::runtime_error("Non-finite comparison output");
                    double d = std::abs(static_cast<double>(a[i]) - b[i]);
                    abs_sum += d;
                    squares += d * d;
                    maximum = std::max(maximum, d);
                    int d8 = std::abs(int(byte(a[i])) - int(byte(b[i])));
                    changed8 += d8 != 0;
                    max8 = std::max(max8, d8);
                    gt_a += std::pow(std::clamp(double(a[i]), 0.0, 1.0) - truth[i], 2);
                    gt_b += std::pow(std::clamp(double(b[i]), 0.0, 1.0) - truth[i], 2);
                }
                const auto ay = luma(a, n), by = luma(b, n);
                Json row = {{"sample", sample.at("id")},
                            {"width", w},
                            {"height", h},
                            {"t", t / 100.0},
                            {"mae", abs_sum / a.size()},
                            {"rmse", std::sqrt(squares / a.size())},
                            {"max_abs", maximum},
                            {"cross_psnr_rgb_db", psnr(squares / a.size())},
                            {"cross_ssim_y", ssim(ay, by, w, h)},
                            {"changed_8bit_channels_fraction", double(changed8) / a.size()},
                            {"max_8bit_channel_difference", max8}};
                if (t == 50)
                    row["held_out_middle_frame"] = {{"rtx_psnr_rgb_db", psnr(gt_a / a.size())},
                                                    {"trt_psnr_rgb_db", psnr(gt_b / a.size())},
                                                    {"rtx_ssim_y", ssim(ay, truth_y, w, h)},
                                                    {"trt_ssim_y", ssim(by, truth_y, w, h)}};
                rows.push_back(row);
                std::cout << row.dump() << '\n';
            }
        }
        Json report = {{"fp32_range", 1.0},
                       {"ssim", "BT.709 full-range luma; 11x11 Gaussian sigma=1.5, valid region"},
                       {"samples", rows}};
        std::ofstream result(argv[3]);
        result << report.dump(2) << '\n';
        double max_abs = 0, max_mae = 0, min_psnr = 1000, min_ssim = 1, max8fraction = 0;
        int max8 = 0;
        for (const auto &r : rows) {
            max_abs = std::max(max_abs, r.at("max_abs").get<double>());
            max_mae = std::max(max_mae, r.at("mae").get<double>());
            if (r.at("cross_psnr_rgb_db").is_number())
                min_psnr = std::min(min_psnr, r.at("cross_psnr_rgb_db").get<double>());
            min_ssim = std::min(min_ssim, r.at("cross_ssim_y").get<double>());
            max8fraction =
                std::max(max8fraction, r.at("changed_8bit_channels_fraction").get<double>());
            max8 = std::max(max8, r.at("max_8bit_channel_difference").get<int>());
        }
        std::ofstream summary(argv[4]);
        summary << Json{{"samples", rows.size()},
                        {"max_abs", max_abs},
                        {"max_mae", max_mae},
                        {"min_cross_psnr_db", min_psnr},
                        {"min_cross_ssim_y", min_ssim},
                        {"max_changed_8bit_channels_fraction", max8fraction},
                        {"max_8bit_channel_difference", max8}}
                       .dump(2)
                << '\n';
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
