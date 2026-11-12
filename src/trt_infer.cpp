/*
 * trt_infer.cpp - see trt_infer.h. Uses the TensorRT 8.5 name-based tensor
 * API (setTensorAddress + enqueueV3).
 */
#include "trt_infer.h"

#include <NvInfer.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
class Logger : public nvinfer1::ILogger {
    void log(Severity s, const char *msg) noexcept override {
        if (s <= Severity::kWARNING) fprintf(stderr, "[trt] %s\n", msg);
    }
} g_logger;

const char *kInput = "data";
const char *kCov   = "Layer7_cov";
const char *kBbox  = "Layer7_bbox";

size_t volume(const nvinfer1::Dims &d) {
    size_t v = 1;
    for (int i = 0; i < d.nbDims; i++) v *= (size_t)d.d[i];
    return v;
}
}  // namespace

struct trt_engine {
    nvinfer1::IRuntime    *runtime = nullptr;
    nvinfer1::ICudaEngine *engine = nullptr;
    int dla_core = -1;
};

struct trt_ctx {
    nvinfer1::IExecutionContext *ctx = nullptr;
    trt_dims_t dims{};
    float *d_in = nullptr, *d_cov = nullptr, *d_bbox = nullptr;
    float *h_cov = nullptr, *h_bbox = nullptr;      /* pinned */
    size_t n_in = 0, n_cov = 0, n_bbox = 0;
};

extern "C" trt_engine_t *trt_engine_load(const char *path, int dla_core)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "[trt] cannot open %s\n", path); return nullptr; }
    std::vector<char> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto *e = new trt_engine;
    e->dla_core = dla_core;
    e->runtime = nvinfer1::createInferRuntime(g_logger);
    /* A DLA engine is built for "a" DLA; the runtime picks which core runs it. */
    if (dla_core >= 0) e->runtime->setDLACore(dla_core);
    e->engine = e->runtime->deserializeCudaEngine(blob.data(), blob.size());
    if (!e->engine) { fprintf(stderr, "[trt] deserialize failed: %s\n", path); trt_engine_free(e); return nullptr; }
    return e;
}

extern "C" void trt_engine_free(trt_engine_t *e)
{
    if (!e) return;
    delete e->engine;
    delete e->runtime;
    delete e;
}

extern "C" trt_ctx_t *trt_ctx_create(trt_engine_t *e)
{
    auto *c = new trt_ctx;
    c->ctx = e->engine->createExecutionContext();
    if (!c->ctx) { trt_ctx_free(c); return nullptr; }

    nvinfer1::Dims in = e->engine->getTensorShape(kInput);
    if (in.d[0] < 0) in.d[0] = 1;                      /* dynamic batch -> 1 */
    c->ctx->setInputShape(kInput, in);
    nvinfer1::Dims cov = c->ctx->getTensorShape(kCov);
    nvinfer1::Dims bb  = c->ctx->getTensorShape(kBbox);
    c->dims = { (int)in.d[1], (int)in.d[2], (int)in.d[3],
                (int)cov.d[1], (int)cov.d[2], (int)cov.d[3] };
    c->n_in = volume(in);
    c->n_cov = volume(cov);
    c->n_bbox = volume(bb);

    bool ok = cudaMalloc((void **)&c->d_in, c->n_in * sizeof(float)) == cudaSuccess &&
              cudaMalloc((void **)&c->d_cov, c->n_cov * sizeof(float)) == cudaSuccess &&
              cudaMalloc((void **)&c->d_bbox, c->n_bbox * sizeof(float)) == cudaSuccess &&
              cudaMallocHost((void **)&c->h_cov, c->n_cov * sizeof(float)) == cudaSuccess &&
              cudaMallocHost((void **)&c->h_bbox, c->n_bbox * sizeof(float)) == cudaSuccess;
    ok = ok && c->ctx->setTensorAddress(kInput, c->d_in) &&
         c->ctx->setTensorAddress(kCov, c->d_cov) &&
         c->ctx->setTensorAddress(kBbox, c->d_bbox);
    if (!ok) { fprintf(stderr, "[trt] context setup failed\n"); trt_ctx_free(c); return nullptr; }
    return c;
}

extern "C" void trt_ctx_free(trt_ctx_t *c)
{
    if (!c) return;
    cudaFree(c->d_in); cudaFree(c->d_cov); cudaFree(c->d_bbox);
    cudaFreeHost(c->h_cov); cudaFreeHost(c->h_bbox);
    delete c->ctx;
    delete c;
}

extern "C" trt_dims_t trt_ctx_dims(const trt_ctx_t *c) { return c->dims; }
extern "C" float *trt_ctx_input(trt_ctx_t *c) { return c->d_in; }
extern "C" const float *trt_ctx_cov(const trt_ctx_t *c) { return c->h_cov; }
extern "C" const float *trt_ctx_bbox(const trt_ctx_t *c) { return c->h_bbox; }

extern "C" int trt_ctx_enqueue(trt_ctx_t *c, cudaStream_t s)
{
    if (!c->ctx->enqueueV3(s)) return -1;
    cudaMemcpyAsync(c->h_cov, c->d_cov, c->n_cov * sizeof(float), cudaMemcpyDeviceToHost, s);
    cudaMemcpyAsync(c->h_bbox, c->d_bbox, c->n_bbox * sizeof(float), cudaMemcpyDeviceToHost, s);
    return 0;
}
