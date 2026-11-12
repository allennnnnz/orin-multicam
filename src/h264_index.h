/*
 * h264_index.h - mmap an Annex-B H.264 elementary stream and index it into
 * access units (one coded picture + its leading SPS/PPS/SEI NAL units).
 *
 * Feeding the decoder one access unit per V4L2 buffer lets the feeder thread
 * pace input exactly like a camera producing one frame every 1/fps seconds.
 */
#ifndef OMC_H264_INDEX_H
#define OMC_H264_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;   /* whole file, mmap'ed read-only (shared by all streams) */
    size_t         size;
    uint32_t       n_au;
    uint64_t      *au_off; /* byte offset of each access unit */
    uint32_t      *au_len;
    uint32_t       n_idr;
    uint32_t      *idr_au; /* access-unit indices that contain an IDR slice */
    uint32_t       max_au_len;
} h264_index_t;

int  h264_index_open(h264_index_t *ix, const char *path);
void h264_index_close(h264_index_t *ix);

#endif
