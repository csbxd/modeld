#include <cuda_runtime.h>
#include <stdint.h>
__global__ void pack_input(const uint8_t *a, const uint8_t *b, float *dst, int count, float t) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    for (int c = 0; c < 3; c++) {
        dst[c * count + i] = a[3 * i + c] / 255.0f;
        dst[(c + 3) * count + i] = b[3 * i + c] / 255.0f;
    }
    dst[6 * count + i] = t;
}
__device__ float bounded(float x) {
    return fminf(fmaxf(x, 0.0f), 1.0f);
}
__device__ uint8_t byte(float x) {
    return static_cast<uint8_t>(min(max(__float2int_rn(x), 0), 255));
}
__global__ void pack_nv12(const float *src, uint8_t *y, uint8_t *uv, int w, int h, int yp,
                          int uvp) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int row = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || row >= h)
        return;
    int n = w * h, i = row * w + x;
    float r = bounded(src[i]), g = bounded(src[n + i]), b = bounded(src[2 * n + i]);
    float l = .2126f * r + .7152f * g + .0722f * b;
    y[row * yp + x] = byte(16.0f + 219.0f * l);
    if ((x % 2) == 0 && (row % 2) == 0) {
        float cb = 0, cr = 0;
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++) {
                int index = (row + j) * w + x + k;
                float rr = bounded(src[index]), gg = bounded(src[n + index]),
                      bb = bounded(src[2 * n + index]);
                float lum = .2126f * rr + .7152f * gg + .0722f * bb;
                cb += (bb - lum) / 1.8556f;
                cr += (rr - lum) / 1.5748f;
            }
        uv[(row / 2) * uvp + x] = byte(128.0f + 56.0f * cb);
        uv[(row / 2) * uvp + x + 1] = byte(128.0f + 56.0f * cr);
    }
}
extern "C" cudaError_t modeld_pack_input(const uint8_t *a, const uint8_t *b, float *dst, int pixels,
                                         float t, cudaStream_t stream) {
    pack_input<<<(pixels + 255) / 256, 256, 0, stream>>>(a, b, dst, pixels, t);
    return cudaGetLastError();
}
extern "C" cudaError_t modeld_pack_nv12(const float *src, uint8_t *y, uint8_t *uv, int w, int h,
                                        int yp, int uvp, cudaStream_t stream) {
    pack_nv12<<<dim3((w + 31) / 32, (h + 7) / 8), dim3(32, 8), 0, stream>>>(src, y, uv, w, h, yp,
                                                                            uvp);
    return cudaGetLastError();
}
