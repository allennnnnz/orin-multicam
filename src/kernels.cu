/*
 * kernels.cu - preprocessing and overlay kernels.
 *
 * Note on portability: pixel data is read as uint8_t, never plain `char`.
 * `char` is unsigned on AArch64 but signed on x86, so code that reads pixels
 * through `char*` silently produces negative values for >127 when the same
 * source is built on x86.
 */
#include "kernels.h"

__global__ void rgba_to_planar(const uint8_t *__restrict__ rgba, int pitch, int w, int h,
                               float *__restrict__ dst, float scale)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    uchar4 p = *reinterpret_cast<const uchar4 *>(rgba + y * pitch + x * 4);
    int plane = w * h, i = y * w + x;
    dst[i]             = p.x * scale;   /* R */
    dst[plane + i]     = p.y * scale;   /* G */
    dst[2 * plane + i] = p.z * scale;   /* B */
}

extern "C" void k_rgba_to_planar(const uint8_t *rgba, int pitch, int w, int h,
                                 float *dst, float scale, cudaStream_t s)
{
    dim3 blk(32, 8);
    dim3 grd((w + blk.x - 1) / blk.x, (h + blk.y - 1) / blk.y);
    rgba_to_planar<<<grd, blk, 0, s>>>(rgba, pitch, w, h, dst, scale);
}

/* One block per box; threads walk the box perimeter band. */
__global__ void draw_boxes(uint8_t *rgba, int pitch, int w, int h,
                           const draw_box_t *boxes, int t)
{
    draw_box_t b = boxes[blockIdx.x];
    int bw = b.x2 - b.x1 + 1, bh = b.y2 - b.y1 + 1;
    if (bw <= 0 || bh <= 0) return;
    int perim = 2 * (bw + bh) * t;
    for (int k = threadIdx.x; k < perim; k += blockDim.x) {
        int band = k / (2 * (bw + bh)), j = k % (2 * (bw + bh)), x, y;
        if (j < bw)                { x = b.x1 + j;            y = b.y1 + band; }
        else if (j < 2 * bw)       { x = b.x1 + (j - bw);     y = b.y2 - band; }
        else if (j < 2 * bw + bh)  { x = b.x1 + band;         y = b.y1 + (j - 2 * bw); }
        else                       { x = b.x2 - band;         y = b.y1 + (j - 2 * bw - bh); }
        if (x >= 0 && x < w && y >= 0 && y < h)
            *reinterpret_cast<uint32_t *>(rgba + y * pitch + x * 4) = b.rgba;
    }
}

extern "C" void k_draw_boxes(uint8_t *rgba, int pitch, int w, int h,
                             const draw_box_t *boxes, int n, int thickness, cudaStream_t s)
{
    if (n <= 0) return;
    draw_boxes<<<n, 256, 0, s>>>(rgba, pitch, w, h, boxes, thickness);
}
