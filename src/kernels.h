/*
 * kernels.h - CUDA kernels callable from C.
 */
#ifndef OMC_KERNELS_H
#define OMC_KERNELS_H

#include <stdint.h>
#include <cuda_runtime_api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t x1, y1, x2, y2;
    uint32_t rgba;      /* little-endian R,G,B,A bytes */
} draw_box_t;

#define MAX_DRAW_BOXES 256

/* RGBA8 pitch-linear -> planar float RGB, value * scale (NCHW, N=1). */
void k_rgba_to_planar(const uint8_t *rgba, int pitch, int w, int h,
                      float *dst, float scale, cudaStream_t s);

/* Draw rectangle outlines into an RGBA8 pitch-linear image.
 * boxes points to device-visible memory (e.g. pinned host). */
void k_draw_boxes(uint8_t *rgba, int pitch, int w, int h,
                  const draw_box_t *boxes, int n, int thickness, cudaStream_t s);

#ifdef __cplusplus
}
#endif
#endif
