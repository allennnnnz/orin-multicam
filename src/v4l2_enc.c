/*
 * v4l2_enc.c - see v4l2_enc.h.
 *
 * Bring-up order matters on Tegra: the CAPTURE (bitstream) format must be set
 * before the OUTPUT (raw) format, and rate-control controls only after both.
 */
#include "v4l2_enc.h"
#include "v4l2_util.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>

#define ENC_DEV     "/dev/nvhost-msenc"
#define N_IN_BUFS   4
#define N_CAP_BUFS  6

struct v4l2_enc {
    int           fd;
    uint32_t      w, h;
    /* output plane (raw frames we fill with VIC) */
    NvBufSurface *in_surf[N_IN_BUFS];
    int           in_used;            /* surfaces handed out at least once */
    /* capture plane (bitstream) */
    void         *cap_ptr[N_CAP_BUFS];
    int           cap_fd[N_CAP_BUFS];
    uint32_t      cap_len[N_CAP_BUFS];
    FILE         *out;
    pthread_t     thread;
    int           thread_started;
    uint64_t      bytes;
    uint32_t      frames;
    int           error;
};

static int queue_cap(v4l2_enc_t *e, uint32_t idx)
{
    struct v4l2_plane pl[1];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.index = idx;
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = pl;
    b.length = 1;
    return xioctl(e->fd, VIDIOC_QBUF, &b);
}

/* Drains encoded bitstream buffers until the zero-length EOS buffer. */
static void *capture_thread(void *arg)
{
    v4l2_enc_t *e = arg;
    for (;;) {
        struct v4l2_plane pl[1];
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        memset(pl, 0, sizeof(pl));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = 1;
        int r = dqbuf_retry(e->fd, &b, 10000);
        if (r == 1) break;
        if (r < 0) { ERRNO("enc: DQBUF capture"); e->error = 1; break; }
        if (pl[0].bytesused == 0) break;          /* EOS */
        fwrite(e->cap_ptr[b.index], 1, pl[0].bytesused, e->out);
        e->bytes += pl[0].bytesused;
        e->frames++;
        if (queue_cap(e, b.index) < 0) { ERRNO("enc: QBUF capture"); e->error = 1; break; }
    }
    return NULL;
}

v4l2_enc_t *enc_open(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate,
                     const char *out_path)
{
    v4l2_enc_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->w = w; e->h = h;
    e->out = fopen(out_path, "wb");
    TRY(e->out, fail, "enc: cannot open %s", out_path);
    e->fd = v4l2_open(ENC_DEV, O_RDWR);
    TRY(e->fd >= 0, fail, "enc: v4l2_open(%s)", ENC_DEV);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;
    TRY(xioctl(e->fd, VIDIOC_S_FMT, &fmt) == 0, fail, "enc: S_FMT capture");

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.num_planes = 2;
    TRY(xioctl(e->fd, VIDIOC_S_FMT, &fmt) == 0, fail, "enc: S_FMT output");

    TRY(set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bitrate) == 0, fail, "enc: bitrate");
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_IDR_INTERVAL, (int32_t)fps);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, (int32_t)fps);
    set_ctrl(e->fd, V4L2_CID_MPEG_VIDEOENC_INSERT_SPS_PPS_AT_IDR, 1);
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = fps;
    xioctl(e->fd, VIDIOC_S_PARM, &parm);

    /* raw input: DMABUF from our own NV12 pitch-linear surfaces */
    struct v4l2_requestbuffers rb = { .count = N_IN_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_DMABUF };
    TRY(xioctl(e->fd, VIDIOC_REQBUFS, &rb) == 0, fail, "enc: REQBUFS output");
    NvBufSurfaceAllocateParams ap;
    memset(&ap, 0, sizeof(ap));
    ap.params.width = w;
    ap.params.height = h;
    ap.params.layout = NVBUF_LAYOUT_PITCH;
    ap.params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
    ap.params.memType = NVBUF_MEM_SURFACE_ARRAY;
    ap.memtag = NvBufSurfaceTag_VIDEO_ENC;
    for (int i = 0; i < N_IN_BUFS; i++) {
        TRY(NvBufSurfaceAllocate(&e->in_surf[i], 1, &ap) == 0, fail, "enc: alloc input");
        e->in_surf[i]->numFilled = 1;
    }

    /* bitstream: MMAP */
    struct v4l2_requestbuffers rc = { .count = N_CAP_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
    TRY(xioctl(e->fd, VIDIOC_REQBUFS, &rc) == 0 && rc.count == N_CAP_BUFS, fail,
        "enc: REQBUFS capture");
    for (uint32_t i = 0; i < N_CAP_BUFS; i++) {
        struct v4l2_plane pl[1];
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        memset(pl, 0, sizeof(pl));
        b.index = i;
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = 1;
        TRY(xioctl(e->fd, VIDIOC_QUERYBUF, &b) == 0, fail, "enc: QUERYBUF");
        e->cap_len[i] = pl[0].length;
        e->cap_ptr[i] = map_mmap_plane(e->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i,
                                       &pl[0], &e->cap_fd[i]);
        TRY(e->cap_ptr[i] != MAP_FAILED, fail, "enc: map capture buf: %s", strerror(errno));
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    TRY(xioctl(e->fd, VIDIOC_STREAMON, &type) == 0, fail, "enc: STREAMON output");
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    TRY(xioctl(e->fd, VIDIOC_STREAMON, &type) == 0, fail, "enc: STREAMON capture");
    for (uint32_t i = 0; i < N_CAP_BUFS; i++)
        TRY(queue_cap(e, i) == 0, fail, "enc: QBUF capture");

    TRY(pthread_create(&e->thread, NULL, capture_thread, e) == 0, fail, "enc: thread");
    e->thread_started = 1;
    LOG("enc: %ux%u H.264 @ %u fps, %u bps -> %s", w, h, fps, bitrate, out_path);
    return e;
fail:
    enc_close(e, NULL, NULL);
    return NULL;
}

NvBufSurface *enc_get_input(v4l2_enc_t *e)
{
    if (e->in_used < N_IN_BUFS) return e->in_surf[e->in_used++];
    struct v4l2_plane pl[2];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.m.planes = pl;
    b.length = 2;
    if (dqbuf_retry(e->fd, &b, 5000) != 0) { ERRNO("enc: DQBUF output"); return NULL; }
    return e->in_surf[b.index];
}

static int queue_in(v4l2_enc_t *e, int idx, int eos, uint64_t ts_us)
{
    struct v4l2_plane pl[2];
    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    memset(pl, 0, sizeof(pl));
    b.index = (uint32_t)idx;
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.m.planes = pl;
    b.length = 2;
    b.timestamp.tv_sec = (time_t)(ts_us / 1000000u);
    b.timestamp.tv_usec = (suseconds_t)(ts_us % 1000000u);
    NvBufSurfaceParams *sp = &e->in_surf[idx]->surfaceList[0];
    for (int p = 0; p < 2; p++) {
        pl[p].m.fd = (int)sp->bufferDesc;            /* one dmabuf, two planes */
        pl[p].bytesused = eos ? 0 : sp->planeParams.psize[p];
    }
    if (xioctl(e->fd, VIDIOC_QBUF, &b) < 0) { ERRNO("enc: QBUF output"); return -1; }
    return 0;
}

int enc_submit(v4l2_enc_t *e, NvBufSurface *s, uint64_t ts_us)
{
    for (int i = 0; i < N_IN_BUFS; i++)
        if (e->in_surf[i] == s) return queue_in(e, i, 0, ts_us);
    ERR("enc: unknown input surface");
    return -1;
}

int enc_close(v4l2_enc_t *e, uint64_t *bytes_written, uint32_t *frames_written)
{
    if (!e) return -1;
    int err = e->error;
    if (e->thread_started) {
        NvBufSurface *s = enc_get_input(e);
        for (int i = 0; s && i < N_IN_BUFS; i++)
            if (e->in_surf[i] == s) queue_in(e, i, 1, 0);   /* EOS */
        pthread_join(e->thread, NULL);
        err |= e->error;
    }
    if (e->fd > 0) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        xioctl(e->fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        xioctl(e->fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < N_CAP_BUFS; i++) {
            if (e->cap_ptr[i] && e->cap_ptr[i] != MAP_FAILED) munmap(e->cap_ptr[i], e->cap_len[i]);
            if (e->cap_fd[i] > 0) close(e->cap_fd[i]);
        }
        v4l2_close(e->fd);
    }
    for (int i = 0; i < N_IN_BUFS; i++)
        if (e->in_surf[i]) NvBufSurfaceDestroy(e->in_surf[i]);
    if (e->out) fclose(e->out);
    if (bytes_written) *bytes_written = e->bytes;
    if (frames_written) *frames_written = e->frames;
    free(e);
    return err ? -1 : 0;
}
