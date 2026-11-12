/*
 * v4l2_dec.h - H.264 decoder on the Tegra NVDEC engine, driven through the
 * V4L2 memory-to-memory API in plain C (no NvVideoDecoder C++ wrapper).
 *
 *   OUTPUT plane  (app -> HW): compressed access units, MMAP buffers
 *   CAPTURE plane (HW -> app): decoded NV12 block-linear frames in
 *                              NvBufSurface dmabufs we allocate (zero copy)
 *
 * Threading: one thread calls dec_feed(), another calls dec_dequeue()/
 * dec_requeue(). The two planes are independent queues on the same fd.
 */
#ifndef OMC_V4L2_DEC_H
#define OMC_V4L2_DEC_H

#include <stdint.h>
#include "nvbufsurface.h"

typedef struct v4l2_dec v4l2_dec_t;

typedef struct {
    int           index;    /* capture-plane buffer index (for requeue) */
    NvBufSurface *surf;     /* decoded frame, NV12 block linear */
    uint64_t      feed_us;  /* timestamp given to dec_feed() for this AU */
} dec_frame_t;

/* complete_frames: 1 if every dec_feed() buffer is exactly one access unit. */
v4l2_dec_t *dec_open(int id, uint32_t max_au_len, int max_perf, int complete_frames);
/* Blocks until an input buffer is free. ts_us travels with the frame. */
int  dec_feed(v4l2_dec_t *d, const uint8_t *au, uint32_t len, uint64_t ts_us);
int  dec_feed_eos(v4l2_dec_t *d);
/* Wait for V4L2_EVENT_RESOLUTION_CHANGE and set up the capture plane. */
int  dec_setup_capture(v4l2_dec_t *d, int timeout_ms, int extra_bufs);
/* 1 = frame, 0 = end of stream, -1 = error. Blocking. */
int  dec_dequeue(v4l2_dec_t *d, dec_frame_t *f);
int  dec_requeue(v4l2_dec_t *d, const dec_frame_t *f);
void dec_get_size(const v4l2_dec_t *d, uint32_t *w, uint32_t *h);
void dec_close(v4l2_dec_t *d);

#endif
