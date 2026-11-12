/*
 * cuda_map.h - give CUDA a device pointer to an NvBufSurface without copying.
 *
 * On Tegra the GPU, VIC, NVDEC and NVENC share the same DRAM. A pitch-linear
 * NvBufSurface is exported as an EGLImage and registered with CUDA once; the
 * resulting pointer is reused for every frame.
 */
#ifndef OMC_CUDA_MAP_H
#define OMC_CUDA_MAP_H

#include <stdint.h>
#include <cuda.h>
#include "nvbufsurface.h"

typedef struct {
    NvBufSurface      *surf;
    CUgraphicsResource res;
    uint8_t           *ptr;     /* device pointer to plane 0 */
    uint32_t           pitch;   /* bytes per row */
    uint32_t           width, height;
} cuda_map_t;

/* Allocate a pitch-linear RGBA surface usable by both VIC and CUDA. */
NvBufSurface *alloc_rgba_surface(uint32_t w, uint32_t h);
/* Long-lived mapping: EGLImage + CUDA registration kept for the surface's
 * lifetime. Fine for buffers CUDA only *reads* after VIC wrote them. */
int  cuda_map_surface(NvBufSurface *s, cuda_map_t *m);
void cuda_unmap_surface(cuda_map_t *m);

/* Short-lived registration of an already EGL-mapped surface, for buffers CUDA
 * *writes* and another engine (VIC/NVENC) reads next: unregistering is the
 * point where the GPU's writes become visible to those engines. */
int  cuda_register(NvBufSurface *s, cuda_map_t *m);
void cuda_unregister(cuda_map_t *m);

#endif
