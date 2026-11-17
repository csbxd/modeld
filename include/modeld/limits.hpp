#pragma once
namespace modeld {
inline constexpr bool supported_dimensions(int width, int height) {
    return width >= 32 && height >= 32 && width % 2 == 0 && height % 2 == 0 &&
           ((width <= 1920 && height <= 1080) || (width <= 1080 && height <= 1920));
}
} // namespace modeld
