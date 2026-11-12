/*
 * v4l2_util.h - helpers shared by the NVDEC decoder and NVENC encoder.
 */
#ifndef OMC_V4L2_UTIL_H
#define OMC_V4L2_UTIL_H

#include <linux/videodev2.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common.h"
#include "libv4l2.h"
#include "v4l2_nv_extensions.h"

static inline int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = v4l2_ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

static inline int set_ctrl(int fd, uint32_t id, int32_t value)
{
    struct v4l2_ext_control c = { .id = id, .value = value };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CTRL_CLASS_MPEG,
                                    .count = 1, .controls = &c };
    return xioctl(fd, VIDIOC_S_EXT_CTRLS, &cs);
}

/*
 * DQBUF with retry. libnvv4l2 in "blocking" mode still returns EAGAIN when its
 * internal wait times out (e.g. the decoder is stalled until the capture plane
 * exists), so the caller must retry. Returns 0 = ok, 1 = LAST/EOS, -1 = error.
 */
static inline int dqbuf_retry(int fd, struct v4l2_buffer *b, int timeout_ms)
{
    uint64_t deadline = now_us() + (uint64_t)timeout_ms * 1000u;
    for (;;) {
        if (v4l2_ioctl(fd, VIDIOC_DQBUF, b) == 0) return 0;
        if (errno == EPIPE) return 1;
        if (errno != EAGAIN && errno != EINTR) return -1;
        if (b->flags & V4L2_BUF_FLAG_LAST) return 1;
        if (now_us() > deadline) { errno = ETIMEDOUT; return -1; }
        usleep(200);
    }
}

/*
 * Map an MMAP-memory V4L2 buffer plane into the process.
 * libnvv4l2 does not implement v4l2_mmap() on the device fd (fails with
 * ENODEV); the buffer has to be exported as a dmabuf and mmap()ed through it.
 */
static inline void *map_mmap_plane(int fd, uint32_t type, uint32_t index,
                                   const struct v4l2_plane *pl, int *dmabuf_fd)
{
    struct v4l2_exportbuffer eb = { .type = type, .index = index, .plane = 0 };
    if (xioctl(fd, VIDIOC_EXPBUF, &eb) < 0) return MAP_FAILED;
    *dmabuf_fd = eb.fd;
    return mmap(NULL, pl->length, PROT_READ | PROT_WRITE, MAP_SHARED, eb.fd,
                pl->m.mem_offset);
}

#endif
