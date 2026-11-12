/*
 * cuda_map.c - see cuda_map.h.
 */
#include "cuda_map.h"
#include "common.h"

#include <cudaEGL.h>

NvBufSurface *alloc_rgba_surface(uint32_t w, uint32_t h)
{
    NvBufSurfaceAllocateParams ap;
    memset(&ap, 0, sizeof(ap));
    ap.params.width = w;
    ap.params.height = h;
    ap.params.layout = NVBUF_LAYOUT_PITCH;
    ap.params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    ap.params.memType = NVBUF_MEM_SURFACE_ARRAY;
    ap.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
    NvBufSurface *s = NULL;
    if (NvBufSurfaceAllocate(&s, 1, &ap) != 0) {
        ERR("NvBufSurfaceAllocate RGBA %ux%u", w, h);
        return NULL;
    }
    s->numFilled = 1;
    return s;
}

int cuda_map_surface(NvBufSurface *s, cuda_map_t *m)
{
    if (NvBufSurfaceMapEglImage(s, 0) != 0) { ERR("NvBufSurfaceMapEglImage"); return -1; }
    if (cuda_register(s, m) < 0) return -1;
    m->surf = s;
    return 0;
}

int cuda_register(NvBufSurface *s, cuda_map_t *m)
{
    memset(m, 0, sizeof(*m));
    CUresult r = cuGraphicsEGLRegisterImage(&m->res, s->surfaceList[0].mappedAddr.eglImage,
                                            CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE);
    if (r != CUDA_SUCCESS) { ERR("cuGraphicsEGLRegisterImage: %d", r); return -1; }
    CUeglFrame f;
    r = cuGraphicsResourceGetMappedEglFrame(&f, m->res, 0, 0);
    if (r != CUDA_SUCCESS || f.frameType != CU_EGL_FRAME_TYPE_PITCH) {
        ERR("cuGraphicsResourceGetMappedEglFrame: %d", r);
        return -1;
    }
    m->ptr = f.frame.pPitch[0];
    m->pitch = f.pitch;
    m->width = f.width;
    m->height = f.height;
    return 0;
}

void cuda_unregister(cuda_map_t *m)
{
    if (m->res) cuGraphicsUnregisterResource(m->res);
    m->res = NULL;
}

void cuda_unmap_surface(cuda_map_t *m)
{
    if (m->res) cuGraphicsUnregisterResource(m->res);
    if (m->surf) NvBufSurfaceUnMapEglImage(m->surf, 0);
    memset(m, 0, sizeof(*m));
}
