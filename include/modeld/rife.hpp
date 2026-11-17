#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
namespace modeld {
struct Image {
    inline static std::atomic<uint64_t> next_id{1};
    uint64_t id = next_id.fetch_add(1);
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;
    double time = 0;
};
class Rife {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Rife(const std::filesystem::path &model, const std::filesystem::path &cache, int width,
         int height);
    ~Rife();
    void enqueue(int slot, const Image &first, const Image &last, float t, uint8_t *y, uint8_t *uv,
                 int y_stride, int uv_stride);
    void wait(int slot);
    int width() const;
    int height() const;
};
} // namespace modeld
