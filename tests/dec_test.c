/*
 * dec_test.c - bring-up test for the C V4L2 decoder + VIC path.
 * Decodes N access units, reports decode FPS, converts the last frame to
 * RGBA (pitch linear) on VIC and writes it as a PPM for visual inspection.
 */
#include <pthread.h>
#include <stdlib.h>

#include "common.h"
#include "h264_index.h"
#include "v4l2_dec.h"
#include "nvbufsurftransform.h"

typedef struct { v4l2_dec_t *dec; h264_index_t *ix; int n; } feeder_arg_t;

static void *feeder(void *p)
{
    feeder_arg_t *a = p;
    for (int i = 0; i < a->n; i++) {
        uint32_t au = (uint32_t)i % a->ix->n_au;
        if (dec_feed(a->dec, a->ix->data + a->ix->au_off[au], a->ix->au_len[au], now_us()) < 0)
            return NULL;
    }
    dec_feed_eos(a->dec);
    return NULL;
}

static int dump_ppm(NvBufSurface *s, const char *path)
{
    if (NvBufSurfaceMap(s, 0, 0, NVBUF_MAP_READ) != 0) return -1;
    NvBufSurfaceSyncForCpu(s, 0, 0);
    NvBufSurfaceParams *p = &s->surfaceList[0];
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%u %u\n255\n", p->width, p->height);
    const uint8_t *base = p->mappedAddr.addr[0];
    for (uint32_t y = 0; y < p->height; y++)
        for (uint32_t x = 0; x < p->width; x++)
            fwrite(base + y * p->planeParams.pitch[0] + x * 4, 1, 3, f);
    fclose(f);
    NvBufSurfaceUnMap(s, 0, 0);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1]
        : "/usr/src/jetson_multimedia_api/data/Video/sample_outdoor_car_1080p_10fps.h264";
    int n = argc > 2 ? atoi(argv[2]) : 200;

    h264_index_t ix;
    if (h264_index_open(&ix, path) < 0) return 1;
    LOG("indexed %u access units, %u IDR, max AU %u bytes", ix.n_au, ix.n_idr, ix.max_au_len);

    v4l2_dec_t *dec = dec_open(0, ix.max_au_len, 1, 1);
    if (!dec) return 1;
    feeder_arg_t fa = { dec, &ix, n };
    pthread_t th;
    pthread_create(&th, NULL, feeder, &fa);
    if (dec_setup_capture(dec, 3000, 2) < 0) return 1;

    uint32_t w, h;
    dec_get_size(dec, &w, &h);
    NvBufSurfaceAllocateParams ap = {0};
    ap.params.width = 640; ap.params.height = 368;
    ap.params.layout = NVBUF_LAYOUT_PITCH;
    ap.params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    ap.params.memType = NVBUF_MEM_SURFACE_ARRAY;
    ap.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
    NvBufSurface *rgba;
    if (NvBufSurfaceAllocate(&rgba, 1, &ap) != 0) return 1;
    rgba->numFilled = 1;

    NvBufSurfTransformConfigParams cfg = { .compute_mode = NvBufSurfTransformCompute_VIC };
    NvBufSurfTransformSetSessionParams(&cfg);
    NvBufSurfTransformParams tp = { .transform_flag = NVBUFSURF_TRANSFORM_FILTER,
                                    .transform_filter = NvBufSurfTransformInter_Bilinear };

    uint64_t t0 = now_us(), vic_us = 0;
    int got = 0;
    dec_frame_t f;
    while (dec_dequeue(dec, &f) == 1) {
        uint64_t a = now_us();
        if (NvBufSurfTransform(f.surf, rgba, &tp) != NvBufSurfTransformError_Success) {
            ERR("VIC transform failed");
            break;
        }
        vic_us += now_us() - a;
        dec_requeue(dec, &f);
        if (++got == n) break;
    }
    double s = (double)(now_us() - t0) / 1e6;
    LOG("decoded %d frames in %.2fs = %.1f fps, VIC 1080p NV12->640x368 RGBA avg %.3f ms",
        got, s, got / s, got ? vic_us / 1000.0 / got : 0);
    dump_ppm(rgba, "dec_test.ppm");
    pthread_join(th, NULL);
    NvBufSurfaceDestroy(rgba);
    dec_close(dec);
    h264_index_close(&ix);
    return got == n ? 0 : 1;
}
