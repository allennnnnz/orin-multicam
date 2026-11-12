/*
 * trt_infer.h - minimal C interface over TensorRT (the only C++ in the app).
 *
 * One trt_engine_t per accelerator (GPU, DLA0, DLA1); one trt_ctx_t per
 * stream, each with its own I/O buffers so streams never share state.
 */
#ifndef OMC_TRT_INFER_H
#define OMC_TRT_INFER_H

#include <cuda_runtime_api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct trt_engine trt_engine_t;
typedef struct trt_ctx    trt_ctx_t;

typedef struct {
    int in_c, in_h, in_w;         /* network input (NCHW, N=1) */
    int n_cls, grid_h, grid_w;    /* coverage output [n_cls, grid_h, grid_w] */
} trt_dims_t;

/* dla_core < 0 selects the GPU. */
trt_engine_t *trt_engine_load(const char *path, int dla_core);
void          trt_engine_free(trt_engine_t *e);

trt_ctx_t *trt_ctx_create(trt_engine_t *e);
void       trt_ctx_free(trt_ctx_t *c);
trt_dims_t trt_ctx_dims(const trt_ctx_t *c);
float     *trt_ctx_input(trt_ctx_t *c);                 /* device pointer */
/* Enqueue inference + async D2H of outputs to pinned host memory. */
int        trt_ctx_enqueue(trt_ctx_t *c, cudaStream_t s);
const float *trt_ctx_cov(const trt_ctx_t *c);           /* valid after stream sync */
const float *trt_ctx_bbox(const trt_ctx_t *c);

#ifdef __cplusplus
}
#endif
#endif
