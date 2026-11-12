/*
 * stats.h - per-frame timestamps for every pipeline stage, percentile
 * summaries, CSV + JSON export. All times are CLOCK_MONOTONIC microseconds.
 */
#ifndef OMC_STATS_H
#define OMC_STATS_H

#include <stdint.h>

typedef struct {
    uint64_t feed;   /* AU handed to NVDEC (the "camera" timestamp)   */
    uint64_t dec;    /* decoded frame dequeued                        */
    uint64_t vic;    /* VIC scale/colour-convert done                 */
    uint64_t inf;    /* preprocess + TensorRT + D2H done              */
    uint64_t post;   /* CPU decode + NMS done: detections available   */
    uint64_t ovl;    /* mosaic tile + box overlay done (0 if no output) */
    uint64_t out;    /* mosaic handed to NVENC (0 if no output)       */
    uint16_t ndet;
} frame_rec_t;

typedef struct {
    char         name[32];   /* e.g. "cam0/dla0" */
    frame_rec_t *rec;
    uint32_t     n;          /* frames recorded */
    uint32_t     cap;
} stream_stats_t;

int  stats_init(stream_stats_t *s, const char *name, uint32_t cap);
void stats_free(stream_stats_t *s);
int  stats_write_csv(const char *path, const stream_stats_t *s, int n_streams);
/* Prints a table and writes a JSON summary (path may be NULL). */
void stats_summary(const stream_stats_t *s, int n_streams, double wall_s,
                   uint32_t warmup, const char *json_path, const char *config_json);

#endif
