/*
 * postprocess.h - decode the ResNet10 (DetectNet-style) grid outputs into
 * boxes and suppress duplicates with greedy NMS. Plain C, runs on a CPU core.
 */
#ifndef OMC_POSTPROCESS_H
#define OMC_POSTPROCESS_H

typedef struct {
    float x1, y1, x2, y2;   /* in network-input pixels */
    float score;
    int   cls;
} det_t;

/* cov:  [n_cls][gh][gw]   bbox: [n_cls*4][gh][gw] (x1,y1,x2,y2 offsets) */
int resnet10_decode(const float *cov, const float *bbox, int n_cls, int gh, int gw,
                    int net_w, int net_h, const float *thresh, det_t *out, int max_out);
int nms(det_t *d, int n, float iou_thresh);

#endif
