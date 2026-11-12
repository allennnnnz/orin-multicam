/*
 * v4l2_enc.h - H.264 encoder on the Tegra NVENC engine via V4L2 M2M (plain C).
 *
 *   OUTPUT plane  (app -> HW): raw NV12 frames, DMABUF (NvBufSurface we own)
 *   CAPTURE plane (HW -> app): H.264 bitstream, MMAP; drained by an internal
 *                              thread and written to a file.
 */
#ifndef OMC_V4L2_ENC_H
#define OMC_V4L2_ENC_H

#include <stdint.h>
#include "nvbufsurface.h"

typedef struct v4l2_enc v4l2_enc_t;

v4l2_enc_t   *enc_open(uint32_t w, uint32_t h, uint32_t fps, uint32_t bitrate,
                       const char *out_path);
/* Blocks until an input surface (NV12 pitch linear) is free to be written. */
NvBufSurface *enc_get_input(v4l2_enc_t *e);
int           enc_submit(v4l2_enc_t *e, NvBufSurface *s, uint64_t ts_us);
/* Sends EOS, waits for the bitstream to drain, closes everything. */
int           enc_close(v4l2_enc_t *e, uint64_t *bytes_written, uint32_t *frames_written);

#endif
