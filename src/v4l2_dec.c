/*
 * v4l2_dec.c - see v4l2_dec.h.
 *
 * Bring-up sequence (mirrors what the NVIDIA multimedia samples do in C++):
 *   open /dev/nvhost-nvdec via libnvv4l2
 *   SUBSCRIBE_EVENT(RESOLUTION_CHANGE)
 *   S_FMT(OUTPUT_MPLANE, H264) -> DISABLE_COMPLETE_FRAME_INPUT -> REQBUFS(MMAP)
 *   STREAMON(OUTPUT) and start feeding
 *   ... HW parses SPS, raises RESOLUTION_CHANGE ...
 *   G_FMT/G_CROP(CAPTURE) -> MIN_BUFFERS_FOR_CAPTURE -> NvBufSurfaceAllocate
 *   REQBUFS(CAPTURE, DMABUF) -> STREAMON(CAPTURE) -> QBUF all capture bufs
 */
#include "v4l2_dec.h"
#include "common.h"

#include <fcntl.h>
#include <stdlib.h>

#include "v4l2_util.h"

#define DEC_DEV        "/dev/nvhost-nvdec"
#define N_OUT_BUFS     4
#define MAX_CAP_BUFS   32

struct v4l2_dec {
    int       id;
    int       fd;
    /* output plane (bitstream) */
    void     *out_ptr[N_OUT_BUFS];
    int       out_fd[N_OUT_BUFS];     /* exported dmabuf fds */
    uint32_t  out_len[N_OUT_BUFS];
    int       out_queued;          /* buffers handed to HW at least once */
    /* capture plane (decoded frames) */
    int           n_cap;
    uint32_t      cap_planes;
    NvBufSurface *cap_surf[MAX_CAP_BUFS];
    uint32_t      width, height;
};

v4l2_dec_t *dec_open(int id, uint32_t max_au_len, int max_perf, int complete_frames)
{
    v4l2_dec_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->id = id;
    d->fd = v4l2_open(DEC_DEV, O_RDWR);   /* blocking: DQBUF sleeps in the driver */
    TRY(d->fd >= 0, fail, "dec%d: v4l2_open(%s) failed", id, DEC_DEV);

    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_RESOLUTION_CHANGE };
    TRY(xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0, fail, "dec%d: subscribe event", id);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.num_planes = 1;
    /* each buffer must hold the largest access unit (IDR frames are big) */
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (max_au_len + 4095u) & ~4095u;
    TRY(xioctl(d->fd, VIDIOC_S_FMT, &fmt) == 0, fail, "dec%d: S_FMT output", id);

    /* complete_frames=1: every OUTPUT buffer holds exactly one access unit, so
     * NVDEC can decode it immediately. With 0 the parser treats input as a byte
     * stream and must see the *next* start code before it knows the picture
     * ended -> one extra frame of latency (33 ms at 30 fps). */
    TRY(set_ctrl(d->fd, V4L2_CID_MPEG_VIDEO_DISABLE_COMPLETE_FRAME_INPUT,
                 complete_frames ? 0 : 1) == 0, fail, "dec%d: frame input mode", id);
    if (max_perf)
        set_ctrl(d->fd, V4L2_CID_MPEG_VIDEO_MAX_PERFORMANCE, 1);

    struct v4l2_requestbuffers rb = {
        .count = N_OUT_BUFS, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP };
    TRY(xioctl(d->fd, VIDIOC_REQBUFS, &rb) == 0 && rb.count == N_OUT_BUFS,
        fail, "dec%d: REQBUFS output", id);

    for (int i = 0; i < N_OUT_BUFS; i++) {
        struct v4l2_plane pl[1];
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        memset(pl, 0, sizeof(pl));
        b.index = (uint32_t)i;
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = 1;
        TRY(xioctl(d->fd, VIDIOC_QUERYBUF, &b) == 0, fail, "dec%d: QUERYBUF", id);
        d->out_len[i] = pl[0].length;
        d->out_ptr[i] = map_mmap_plane(d->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                       (uint32_t)i, &pl[0], &d->out_fd[i]);
        TRY(d->out_ptr[i] != MAP_FAILED, fail, "dec%d: mmap output buf: %s", id, strerror(errno));
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    TRY(xioctl(d->fd, VIDIOC_STREAMON, &type) == 0, fail, "dec%d: STREAMON output", id);
    return d;
fail:
    dec_close(d);
    return NULL;
}

static int queue_out(v4l2_dec_t *d, int idx, uint32_t bytes, uint64_t ts_us)
{
    struct v4l2_plane pl[1];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.index = (uint32_t)idx;
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = pl;
    b.length = 1;
    pl[0].bytesused = bytes;
    /* The decoder copies the OUTPUT timestamp onto the matching CAPTURE buffer,
     * which is how per-frame latency is tracked through the HW engine. */
    b.timestamp.tv_sec = (time_t)(ts_us / 1000000u);
    b.timestamp.tv_usec = (suseconds_t)(ts_us % 1000000u);
    if (xioctl(d->fd, VIDIOC_QBUF, &b) < 0) { ERRNO("dec%d: QBUF output", d->id); return -1; }
    return 0;
}

static int get_free_out(v4l2_dec_t *d)
{
    if (d->out_queued < N_OUT_BUFS) return d->out_queued++;
    struct v4l2_plane pl[1];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = pl;
    b.length = 1;
    if (dqbuf_retry(d->fd, &b, 5000) != 0) { ERRNO("dec%d: DQBUF output", d->id); return -1; }
    return (int)b.index;
}

int dec_feed(v4l2_dec_t *d, const uint8_t *au, uint32_t len, uint64_t ts_us)
{
    int idx = get_free_out(d);
    if (idx < 0) return -1;
    if (len > d->out_len[idx]) { ERR("dec%d: AU %u > buffer %u", d->id, len, d->out_len[idx]); return -1; }
    memcpy(d->out_ptr[idx], au, len);
    return queue_out(d, idx, len, ts_us);
}

int dec_feed_eos(v4l2_dec_t *d)
{
    int idx = get_free_out(d);
    if (idx < 0) return -1;
    return queue_out(d, idx, 0, 0);   /* bytesused == 0 signals EOS */
}

int dec_setup_capture(v4l2_dec_t *d, int timeout_ms, int extra_bufs)
{
    struct v4l2_event ev;
    int waited = 0;
    for (;;) {
        memset(&ev, 0, sizeof(ev));
        if (xioctl(d->fd, VIDIOC_DQEVENT, &ev) == 0 && ev.type == V4L2_EVENT_RESOLUTION_CHANGE)
            break;
        if (waited++ >= timeout_ms) { ERR("dec%d: no RESOLUTION_CHANGE event", d->id); return -1; }
        usleep(1000);
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_G_FMT, &fmt) < 0) { ERRNO("dec%d: G_FMT capture", d->id); return -1; }
    d->cap_planes = fmt.fmt.pix_mp.num_planes;

    /* coded size may be 1088 high; the crop rectangle is the visible 1080 */
    struct v4l2_crop crop;
    memset(&crop, 0, sizeof(crop));
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_G_CROP, &crop) < 0) { ERRNO("dec%d: G_CROP", d->id); return -1; }
    d->width = crop.c.width;
    d->height = crop.c.height;

    struct v4l2_control ctl = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    if (xioctl(d->fd, VIDIOC_G_CTRL, &ctl) < 0) { ERRNO("dec%d: MIN_BUFFERS", d->id); return -1; }
    d->n_cap = ctl.value + extra_bufs;
    if (d->n_cap > MAX_CAP_BUFS) d->n_cap = MAX_CAP_BUFS;

    NvBufSurfaceAllocateParams ap;
    memset(&ap, 0, sizeof(ap));
    ap.params.width = d->width;
    ap.params.height = d->height;
    ap.params.layout = NVBUF_LAYOUT_BLOCK_LINEAR;   /* native NVDEC/VIC tiling */
    ap.params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
    ap.params.memType = NVBUF_MEM_SURFACE_ARRAY;
    ap.memtag = NvBufSurfaceTag_VIDEO_DEC;
    for (int i = 0; i < d->n_cap; i++) {
        if (NvBufSurfaceAllocate(&d->cap_surf[i], 1, &ap) != 0) {
            ERR("dec%d: NvBufSurfaceAllocate", d->id);
            return -1;
        }
        d->cap_surf[i]->numFilled = 1;
    }

    struct v4l2_requestbuffers rb = {
        .count = (uint32_t)d->n_cap, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_DMABUF };
    if (xioctl(d->fd, VIDIOC_REQBUFS, &rb) < 0) { ERRNO("dec%d: REQBUFS capture", d->id); return -1; }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(d->fd, VIDIOC_STREAMON, &type) < 0) { ERRNO("dec%d: STREAMON capture", d->id); return -1; }

    for (int i = 0; i < d->n_cap; i++) {
        dec_frame_t f = { .index = i, .surf = d->cap_surf[i] };
        if (dec_requeue(d, &f) < 0) return -1;
    }
    LOG("dec%d: %ux%u, %d capture buffers (%u planes)", d->id, d->width, d->height,
        d->n_cap, d->cap_planes);
    return 0;
}

int dec_dequeue(v4l2_dec_t *d, dec_frame_t *f)
{
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.m.planes = pl;
    b.length = d->cap_planes;
    int r = dqbuf_retry(d->fd, &b, 5000);
    if (r == 1) return 0;                        /* drained after EOS */
    if (r < 0) { ERRNO("dec%d: DQBUF capture", d->id); return -1; }
    if ((b.flags & V4L2_BUF_FLAG_LAST) && pl[0].bytesused == 0) return 0;
    f->index = (int)b.index;
    f->surf = d->cap_surf[b.index];
    f->feed_us = (uint64_t)b.timestamp.tv_sec * 1000000u + (uint64_t)b.timestamp.tv_usec;
    return 1;
}

int dec_requeue(v4l2_dec_t *d, const dec_frame_t *f)
{
    struct v4l2_plane pl[VIDEO_MAX_PLANES];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.index = (uint32_t)f->index;
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.m.planes = pl;
    b.length = d->cap_planes;
    for (uint32_t p = 0; p < d->cap_planes; p++)
        pl[p].m.fd = (int)d->cap_surf[f->index]->surfaceList[0].bufferDesc;
    if (xioctl(d->fd, VIDIOC_QBUF, &b) < 0) { ERRNO("dec%d: QBUF capture", d->id); return -1; }
    return 0;
}

void dec_get_size(const v4l2_dec_t *d, uint32_t *w, uint32_t *h)
{
    *w = d->width;
    *h = d->height;
}

void dec_close(v4l2_dec_t *d)
{
    if (!d) return;
    if (d->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(d->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(d->fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < N_OUT_BUFS; i++)
            if (d->out_ptr[i] && d->out_ptr[i] != MAP_FAILED)
                munmap(d->out_ptr[i], d->out_len[i]);
        for (int i = 0; i < N_OUT_BUFS; i++)
            if (d->out_fd[i] > 0) close(d->out_fd[i]);
        struct v4l2_requestbuffers rb = { .count = 0, .memory = V4L2_MEMORY_DMABUF,
                                          .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
        xioctl(d->fd, VIDIOC_REQBUFS, &rb);
        v4l2_close(d->fd);
    }
    for (int i = 0; i < d->n_cap; i++)
        if (d->cap_surf[i]) NvBufSurfaceDestroy(d->cap_surf[i]);
    free(d);
}
