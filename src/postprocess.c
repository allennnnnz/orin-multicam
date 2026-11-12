/*
 * postprocess.c - see postprocess.h.
 *
 * Each grid cell (stride 16) predicts a coverage score per class and four box
 * offsets normalised by 35 relative to the cell centre (same decoding as the
 * DeepStream/MMAPI ResNet10 parser).
 */
#include "postprocess.h"

#include <stdlib.h>

#define STRIDE    16.0f
#define BBOX_NORM 35.0f

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int resnet10_decode(const float *cov, const float *bbox, int n_cls, int gh, int gw,
                    int net_w, int net_h, const float *thresh, det_t *out, int max_out)
{
    int n = 0, grid = gh * gw;
    for (int c = 0; c < n_cls; c++) {
        const float *pc = cov + c * grid;
        const float *bx1 = bbox + (c * 4 + 0) * grid;
        const float *by1 = bbox + (c * 4 + 1) * grid;
        const float *bx2 = bbox + (c * 4 + 2) * grid;
        const float *by2 = bbox + (c * 4 + 3) * grid;
        for (int y = 0; y < gh; y++) {
            float cy = (y * STRIDE + 0.5f) / BBOX_NORM;
            for (int x = 0; x < gw; x++) {
                int i = y * gw + x;
                if (pc[i] < thresh[c] || n >= max_out) continue;
                float cx = (x * STRIDE + 0.5f) / BBOX_NORM;
                det_t *d = &out[n++];
                d->x1 = clampf((bx1[i] - cx) * -BBOX_NORM, 0, (float)net_w - 1);
                d->y1 = clampf((by1[i] - cy) * -BBOX_NORM, 0, (float)net_h - 1);
                d->x2 = clampf((bx2[i] + cx) * BBOX_NORM, 0, (float)net_w - 1);
                d->y2 = clampf((by2[i] + cy) * BBOX_NORM, 0, (float)net_h - 1);
                d->score = pc[i];
                d->cls = c;
            }
        }
    }
    return n;
}

static int by_score_desc(const void *a, const void *b)
{
    float sa = ((const det_t *)a)->score, sb = ((const det_t *)b)->score;
    return (sa < sb) - (sa > sb);
}

static float iou(const det_t *a, const det_t *b)
{
    float ix = (a->x2 < b->x2 ? a->x2 : b->x2) - (a->x1 > b->x1 ? a->x1 : b->x1);
    float iy = (a->y2 < b->y2 ? a->y2 : b->y2) - (a->y1 > b->y1 ? a->y1 : b->y1);
    if (ix <= 0 || iy <= 0) return 0;
    float inter = ix * iy;
    float ua = (a->x2 - a->x1) * (a->y2 - a->y1) + (b->x2 - b->x1) * (b->y2 - b->y1) - inter;
    return ua > 0 ? inter / ua : 0;
}

int nms(det_t *d, int n, float iou_thresh)
{
    qsort(d, (size_t)n, sizeof(*d), by_score_desc);
    int kept = 0;
    for (int i = 0; i < n; i++) {
        int keep = 1;
        for (int j = 0; j < kept && keep; j++)
            if (d[j].cls == d[i].cls && iou(&d[j], &d[i]) > iou_thresh) keep = 0;
        if (keep) d[kept++] = d[i];
    }
    return kept;
}
