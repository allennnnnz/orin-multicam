/*
 * main.c - orin-multicam: N-camera perception pipeline on Jetson AGX Orin.
 *
 *  per stream ("camera"):
 *    feeder thread : paces H.264 access units at --fps into NVDEC (V4L2)
 *    worker thread : NVDEC frame -> VIC scale/CSC -> CUDA preprocess ->
 *                    TensorRT on GPU / DLA0 / DLA1 -> CPU decode+NMS ->
 *                    (optional) VIC tile into mosaic + CUDA box overlay
 *  main thread (when --output): waits for all cameras' frame n (frame sync),
 *                    VIC RGBA->NV12, hands the mosaic to NVENC.
 *
 *  Every image buffer is an NvBufSurface dmabuf: NVDEC, VIC, GPU, DLA and
 *  NVENC all work on the same DRAM; the CPU never touches pixels.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cuda_runtime_api.h>

#include "common.h"
#include "cuda_map.h"
#include "h264_index.h"
#include "kernels.h"
#include "nvbufsurftransform.h"
#include "postprocess.h"
#include "stats.h"
#include "trt_infer.h"
#include "v4l2_dec.h"
#include "v4l2_enc.h"

#define MOSAIC_W   1920
#define MOSAIC_H   1080
#define N_SLOTS    3        /* mosaic frames in flight */
#define MAX_DETS   1024
#define MAX_CLS    8

/* ------------------------------------------------------------------ options */
typedef struct {
    const char *video, *gpu_engine, *dla_engine, *output, *csv, *json, *labels;
    char        engines[MAX_STREAMS][8];
    int         streams, frames, warmup, rt, pin, max_perf, same_scene, chunked;
    double      fps, thresh;
} opts_t;

static opts_t g_opt = {
    .video      = "data/camera_ipp_1080p.h264",
    .gpu_engine = "models/resnet10_gpu_int8_fix.engine",
    .dla_engine = "models/resnet10_dla_int8_fix.engine",
    .labels     = "models/labels.txt",
    .engines    = { "gpu", "dla0", "dla1", "gpu" },
    .streams = 4, .frames = 300, .warmup = 20, .fps = 30.0, .thresh = 0.4,
    .max_perf = 1,
};

/* ------------------------------------------------------------------ state */
typedef struct {
    NvBufSurface   *surf[N_SLOTS];
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             slot_frame[N_SLOTS];  /* frame number a slot is open for */
    int             done[N_SLOTS];        /* cameras finished with that frame */
} mosaic_t;

typedef struct {
    int             id, dla_core, flip, cpu;
    char            engine[8];
    uint32_t        start_au;
    v4l2_dec_t     *dec;
    trt_ctx_t      *trt;
    cudaStream_t    cs;
    NvBufSurface   *infer_surf;
    cuda_map_t      infer_map;
    draw_box_t     *boxes;                /* pinned, read by the draw kernel */
    stream_stats_t  st;
    uint32_t        cls_count[MAX_CLS];
    int             failed;
    pthread_t       feeder, worker;
} stream_t;

static h264_index_t     g_ix;
static stream_t         g_s[MAX_STREAMS];
static mosaic_t         g_mos;
static uint64_t         g_t0;
static volatile int     g_stop;
static char             g_labels[MAX_CLS][32];
static int              g_n_labels;

static const uint32_t k_cls_rgba[MAX_CLS] = {   /* bytes R,G,B,A (little endian) */
    0xff3030ffu, 0xff30ff30u, 0xffff8030u, 0xff30ffffu,
    0xffff30ffu, 0xffffff30u, 0xff8080ffu, 0xffffffffu };

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Condition wait that also wakes up periodically, so a stop request from a
 * signal handler or a failing thread can never leave a waiter blocked. */
static void wait_cv(pthread_cond_t *cv, pthread_mutex_t *mu)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(cv, mu, &ts);
}

static void request_stop(void)
{
    g_stop = 1;
    if (g_opt.output) {
        pthread_mutex_lock(&g_mos.mu);
        pthread_cond_broadcast(&g_mos.cv);
        pthread_mutex_unlock(&g_mos.mu);
    }
}

static void set_vic_session(void)
{
    /* NvBufSurfTransform session params are per thread. */
    NvBufSurfTransformConfigParams cfg = { .compute_mode = NvBufSurfTransformCompute_VIC };
    NvBufSurfTransformSetSessionParams(&cfg);
}

static void tune_thread(int cpu, int prio, const char *name)
{
    pthread_setname_np(pthread_self(), name);
    if (g_opt.pin && cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
            LOG("%s: cannot pin to cpu%d", name, cpu);
    }
    if (g_opt.rt) {
        struct sched_param sp = { .sched_priority = prio };
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            LOG("%s: SCHED_FIFO denied (run as root for --rt)", name);
    }
}

/* ------------------------------------------------------------------ mosaic */
static int mosaic_init(void)
{
    pthread_mutex_init(&g_mos.mu, NULL);
    pthread_cond_init(&g_mos.cv, NULL);
    for (int k = 0; k < N_SLOTS; k++) {
        g_mos.surf[k] = alloc_rgba_surface(MOSAIC_W, MOSAIC_H);
        if (!g_mos.surf[k]) return -1;
        NvBufSurfaceMemSet(g_mos.surf[k], 0, 0, 0);
        if (NvBufSurfaceMapEglImage(g_mos.surf[k], 0) != 0) return -1;
        g_mos.slot_frame[k] = k;
    }
    return 0;
}

static int mosaic_acquire(int frame)
{
    int k = frame % N_SLOTS;
    pthread_mutex_lock(&g_mos.mu);
    while (g_mos.slot_frame[k] != frame && !g_stop)
        wait_cv(&g_mos.cv, &g_mos.mu);
    pthread_mutex_unlock(&g_mos.mu);
    return k;
}

static void mosaic_release(int frame)
{
    pthread_mutex_lock(&g_mos.mu);
    g_mos.done[frame % N_SLOTS]++;
    pthread_cond_broadcast(&g_mos.cv);
    pthread_mutex_unlock(&g_mos.mu);
}

static NvBufSurfTransformRect tile_rect(int id)
{
    NvBufSurfTransformRect r = { .top = (uint32_t)(id / 2) * (MOSAIC_H / 2),
                                 .left = (uint32_t)(id % 2) * (MOSAIC_W / 2),
                                 .width = MOSAIC_W / 2, .height = MOSAIC_H / 2 };
    return r;
}

/* ------------------------------------------------------------------ feeder */
static void *feeder_main(void *arg)
{
    stream_t *s = arg;
    char name[16];
    snprintf(name, sizeof(name), "feed%d", s->id);
    tune_thread(s->cpu, 60, name);

    uint64_t period = g_opt.fps > 0 ? (uint64_t)(1e6 / g_opt.fps) : 0;
    for (int i = 0; i < g_opt.frames && !g_stop; i++) {
        if (period) {   /* all cameras share g_t0: they "expose" in lock-step */
            uint64_t due = g_t0 + (uint64_t)i * period, now = now_us();
            if (due > now) {
                struct timespec ts = { .tv_sec = (time_t)(due / 1000000u),
                                       .tv_nsec = (long)(due % 1000000u) * 1000 };
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
            }
        }
        uint32_t au = (s->start_au + (uint32_t)i) % g_ix.n_au;
        if (dec_feed(s->dec, g_ix.data + g_ix.au_off[au], g_ix.au_len[au], now_us()) < 0) {
            s->failed = 1;
            request_stop();
            break;
        }
    }
    dec_feed_eos(s->dec);
    return NULL;
}

/* ------------------------------------------------------------------ worker */
static void *worker_main(void *arg)
{
    stream_t *s = arg;
    char name[16];
    snprintf(name, sizeof(name), "work%d", s->id);
    tune_thread(s->cpu, 50, name);
    set_vic_session();

    trt_dims_t dm = trt_ctx_dims(s->trt);
    float thr[MAX_CLS];
    for (int c = 0; c < MAX_CLS; c++) thr[c] = (float)g_opt.thresh;
    static __thread det_t dets[MAX_DETS];

    if (dec_setup_capture(s->dec, 5000, 4) < 0) { s->failed = 1; request_stop(); return NULL; }

    NvBufSurfTransformParams tp_inf = {
        .transform_flag = NVBUFSURF_TRANSFORM_FILTER | (s->flip ? NVBUFSURF_TRANSFORM_FLIP : 0),
        .transform_flip = s->flip ? NvBufSurfTransform_FlipX : NvBufSurfTransform_None,
        .transform_filter = NvBufSurfTransformInter_Bilinear };
    NvBufSurfTransformRect tile = tile_rect(s->id);
    NvBufSurfTransformParams tp_tile = tp_inf;
    tp_tile.transform_flag |= NVBUFSURF_TRANSFORM_CROP_DST;
    tp_tile.dst_rect = &tile;

    for (int n = 0; n < g_opt.frames && !g_stop; n++) {
        frame_rec_t *r = &s->st.rec[n];
        dec_frame_t f;
        int got = dec_dequeue(s->dec, &f);
        if (got != 1) { if (got < 0) s->failed = 1; break; }
        r->feed = f.feed_us;
        r->dec = now_us();

        if (NvBufSurfTransform(f.surf, s->infer_surf, &tp_inf) != NvBufSurfTransformError_Success) {
            ERR("cam%d: VIC transform", s->id); s->failed = 1; break;
        }
        r->vic = now_us();

        k_rgba_to_planar(s->infer_map.ptr, (int)s->infer_map.pitch, dm.in_w, dm.in_h,
                         trt_ctx_input(s->trt), 1.0f / 255.0f, s->cs);
        if (trt_ctx_enqueue(s->trt, s->cs) < 0) { ERR("cam%d: enqueue", s->id); s->failed = 1; break; }
        cudaStreamSynchronize(s->cs);
        r->inf = now_us();

        int nd = resnet10_decode(trt_ctx_cov(s->trt), trt_ctx_bbox(s->trt), dm.n_cls,
                                 dm.grid_h, dm.grid_w, dm.in_w, dm.in_h, thr, dets, MAX_DETS);
        nd = nms(dets, nd, 0.45f);
        r->ndet = (uint16_t)nd;
        for (int i = 0; i < nd; i++)
            if (dets[i].cls < MAX_CLS) s->cls_count[dets[i].cls]++;
        r->post = now_us();                  /* detections available */

        /* Visualisation path, measured separately so it never inflates the
         * perception stages: wait for this frame's mosaic slot, VIC-scale the
         * decoded frame into our tile, then draw boxes on it with CUDA. */
        if (g_opt.output) {
            int slot = mosaic_acquire(n);
            if (g_stop) { dec_requeue(s->dec, &f); break; }
            NvBufSurfTransform(f.surf, g_mos.surf[slot], &tp_tile);
            float sx = (float)tile.width / dm.in_w, sy = (float)tile.height / dm.in_h;
            int nb = nd < MAX_DRAW_BOXES ? nd : MAX_DRAW_BOXES;
            for (int i = 0; i < nb; i++) {
                s->boxes[i] = (draw_box_t){ (int16_t)(dets[i].x1 * sx), (int16_t)(dets[i].y1 * sy),
                                            (int16_t)(dets[i].x2 * sx), (int16_t)(dets[i].y2 * sy),
                                            k_cls_rgba[dets[i].cls % MAX_CLS] };
            }
            /* CUDA writes that VIC/NVENC must see: register the tile's EGLImage
             * for this access only. Unregistering is what makes the GPU's
             * writes visible to the other engines; with a long-lived
             * registration ~35% of frames lost their boxes (debug-log #7). */
            cuda_map_t mm, *m = &mm;
            if (cuda_register(g_mos.surf[slot], m) < 0) { s->failed = 1; request_stop(); break; }
            k_draw_boxes(m->ptr + tile.top * m->pitch + tile.left * 4, (int)m->pitch,
                         (int)tile.width, (int)tile.height, s->boxes, nb, 3, s->cs);
            cudaStreamSynchronize(s->cs);
            cuda_unregister(m);
            r->ovl = now_us();
            mosaic_release(n);
        }
        dec_requeue(s->dec, &f);             /* NVDEC can reuse the buffer now */
        s->st.n = (uint32_t)n + 1;
    }
    if (s->st.n < (uint32_t)g_opt.frames) {   /* error or early EOS: stop everyone */
        s->failed |= !g_stop;
        request_stop();
    }
    return NULL;
}

/* ------------------------------------------------------------------ main */
static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --video PATH         H.264 Annex-B input (default: data/camera_ipp_1080p.h264)\n"
        "  --streams N          number of cameras, 1..4 (default 4)\n"
        "  --engines LIST       accelerator per camera: gpu|dla0|dla1 (default gpu,dla0,dla1,gpu)\n"
        "  --gpu-engine PATH    TensorRT engine built for GPU\n"
        "  --dla-engine PATH    TensorRT engine built for DLA\n"
        "  --frames N           frames per camera (default 300)\n"
        "  --fps F              camera frame rate; 0 = run as fast as possible (default 30)\n"
        "  --thresh T           detection threshold (default 0.4)\n"
        "  --output FILE.h264   write 2x2 annotated mosaic via NVENC\n"
        "  --csv FILE           per-frame stage timestamps\n"
        "  --json FILE          summary statistics\n"
        "  --warmup N           frames excluded from statistics (default 20)\n"
        "  --rt                 SCHED_FIFO + mlockall (needs root)\n"
        "  --pin                pin each camera's threads to its own CPU core\n"
        "  --no-max-perf        leave NVDEC at default clocking\n"
        "  --chunked-input      feed NVDEC as a byte stream (legacy, +1 frame latency)\n"
        "  --same-scene         all cameras see identical frames (accuracy A/B)\n", p);
}

static int parse_args(int argc, char **argv)
{
    enum { O_VIDEO = 1, O_STREAMS, O_ENGINES, O_GPU, O_DLA, O_FRAMES, O_FPS, O_THRESH,
           O_OUTPUT, O_CSV, O_JSON, O_WARMUP, O_RT, O_PIN, O_NOMAX, O_SAME, O_CHUNK, O_HELP };
    static const struct option lo[] = {
        {"video", 1, 0, O_VIDEO}, {"streams", 1, 0, O_STREAMS}, {"engines", 1, 0, O_ENGINES},
        {"gpu-engine", 1, 0, O_GPU}, {"dla-engine", 1, 0, O_DLA}, {"frames", 1, 0, O_FRAMES},
        {"fps", 1, 0, O_FPS}, {"thresh", 1, 0, O_THRESH}, {"output", 1, 0, O_OUTPUT},
        {"csv", 1, 0, O_CSV}, {"json", 1, 0, O_JSON}, {"warmup", 1, 0, O_WARMUP},
        {"rt", 0, 0, O_RT}, {"pin", 0, 0, O_PIN}, {"no-max-perf", 0, 0, O_NOMAX},
        {"same-scene", 0, 0, O_SAME}, {"chunked-input", 0, 0, O_CHUNK},
        {"help", 0, 0, O_HELP}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (c) {
        case O_VIDEO:   g_opt.video = optarg; break;
        case O_STREAMS: g_opt.streams = atoi(optarg); break;
        case O_ENGINES: {
            char *dup = strdup(optarg), *save = NULL, *tok = strtok_r(dup, ",", &save);
            for (int i = 0; tok && i < MAX_STREAMS; i++, tok = strtok_r(NULL, ",", &save))
                snprintf(g_opt.engines[i], sizeof(g_opt.engines[i]), "%s", tok);
            free(dup);
            break; }
        case O_GPU:     g_opt.gpu_engine = optarg; break;
        case O_DLA:     g_opt.dla_engine = optarg; break;
        case O_FRAMES:  g_opt.frames = atoi(optarg); break;
        case O_FPS:     g_opt.fps = atof(optarg); break;
        case O_THRESH:  g_opt.thresh = atof(optarg); break;
        case O_OUTPUT:  g_opt.output = optarg; break;
        case O_CSV:     g_opt.csv = optarg; break;
        case O_JSON:    g_opt.json = optarg; break;
        case O_WARMUP:  g_opt.warmup = atoi(optarg); break;
        case O_RT:      g_opt.rt = 1; break;
        case O_PIN:     g_opt.pin = 1; break;
        case O_NOMAX:   g_opt.max_perf = 0; break;
        case O_SAME:    g_opt.same_scene = 1; break;
        case O_CHUNK:   g_opt.chunked = 1; break;
        default:        usage(argv[0]); return -1;
        }
    }
    if (g_opt.streams < 1 || g_opt.streams > MAX_STREAMS || g_opt.frames < 1) {
        usage(argv[0]);
        return -1;
    }
    return 0;
}

static void load_labels(void)
{
    FILE *f = fopen(g_opt.labels, "r");
    for (int i = 0; i < MAX_CLS; i++) snprintf(g_labels[i], sizeof(g_labels[i]), "class%d", i);
    if (!f) return;
    char line[64];
    while (g_n_labels < MAX_CLS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(g_labels[g_n_labels++], sizeof(g_labels[0]), "%.31s", line);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (parse_args(argc, argv) < 0) return 2;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (g_opt.rt && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        LOG("mlockall failed (run as root for --rt)");
    load_labels();

    int ret = 1, n = g_opt.streams;
    trt_engine_t *eng_gpu = NULL, *eng_dla[2] = { NULL, NULL };
    v4l2_enc_t *enc = NULL;

    TRY(h264_index_open(&g_ix, g_opt.video) == 0, out, "cannot index %s", g_opt.video);
    LOG("input: %s (%u frames, %u IDR)", g_opt.video, g_ix.n_au, g_ix.n_idr);

    cudaFree(0);                         /* create the primary CUDA context */
    set_vic_session();

    for (int i = 0; i < n; i++) {
        stream_t *s = &g_s[i];
        s->id = i;
        s->cpu = 1 + i;                  /* core 0 left for the OS + compositor */
        snprintf(s->engine, sizeof(s->engine), "%s", g_opt.engines[i]);
        s->dla_core = !strcmp(s->engine, "dla0") ? 0 : !strcmp(s->engine, "dla1") ? 1 : -1;
        TRY(s->dla_core >= 0 || !strcmp(s->engine, "gpu"), out, "unknown engine '%s'", s->engine);
        /* cameras 0..2 start at different IDRs; camera 3 is a mirrored "rear" view */
        uint32_t idr = g_ix.n_idr >= (uint32_t)n ? (uint32_t)i * g_ix.n_idr / (uint32_t)n
                                                 : (uint32_t)i % g_ix.n_idr;
        s->start_au = g_opt.same_scene ? 0 : g_ix.idr_au[idr];
        s->flip = (i == 3) && !g_opt.same_scene;

        trt_engine_t **slot = s->dla_core < 0 ? &eng_gpu : &eng_dla[s->dla_core];
        if (!*slot) {
            *slot = trt_engine_load(s->dla_core < 0 ? g_opt.gpu_engine : g_opt.dla_engine,
                                    s->dla_core);
            TRY(*slot, out, "cannot load engine for %s", s->engine);
        }
        s->trt = trt_ctx_create(*slot);
        TRY(s->trt, out, "cam%d: TensorRT context", i);
        cudaStreamCreateWithFlags(&s->cs, cudaStreamNonBlocking);
        trt_dims_t dm = trt_ctx_dims(s->trt);
        s->infer_surf = alloc_rgba_surface((uint32_t)dm.in_w, (uint32_t)dm.in_h);
        TRY(s->infer_surf && cuda_map_surface(s->infer_surf, &s->infer_map) == 0, out,
            "cam%d: inference surface", i);
        cudaMallocHost((void **)&s->boxes, MAX_DRAW_BOXES * sizeof(draw_box_t));
        char nm[32];
        snprintf(nm, sizeof(nm), "cam%d/%s", i, s->engine);
        TRY(stats_init(&s->st, nm, (uint32_t)g_opt.frames) == 0, out, "stats");
        s->dec = dec_open(i, g_ix.max_au_len, g_opt.max_perf, !g_opt.chunked);
        TRY(s->dec, out, "cam%d: decoder", i);
        LOG("cam%d: engine=%s start_au=%u flip=%d net=%dx%d", i, s->engine, s->start_au,
            s->flip, dm.in_w, dm.in_h);
    }

    if (g_opt.output) {
        TRY(mosaic_init() == 0, out, "mosaic init");
        enc = enc_open(MOSAIC_W, MOSAIC_H, g_opt.fps > 0 ? (uint32_t)(g_opt.fps + 0.5) : 30,
                       8000000, g_opt.output);
        TRY(enc, out, "encoder");
    }

    g_t0 = now_us() + 20000;             /* common "shutter" epoch for all cameras */
    for (int i = 0; i < n; i++) {
        pthread_create(&g_s[i].worker, NULL, worker_main, &g_s[i]);
        pthread_create(&g_s[i].feeder, NULL, feeder_main, &g_s[i]);
    }

    if (g_opt.output) {                  /* compositor: frame-synchronous mosaic */
        NvBufSurfTransformParams tp = { .transform_flag = NVBUFSURF_TRANSFORM_FILTER,
                                        .transform_filter = NvBufSurfTransformInter_Nearest };
        for (int f = 0; f < g_opt.frames && !g_stop; f++) {
            int k = f % N_SLOTS;
            pthread_mutex_lock(&g_mos.mu);
            while (g_mos.done[k] < n && !g_stop)
                wait_cv(&g_mos.cv, &g_mos.mu);
            pthread_mutex_unlock(&g_mos.mu);
            if (g_stop) break;
            NvBufSurface *in = enc_get_input(enc);
            if (!in || NvBufSurfTransform(g_mos.surf[k], in, &tp) != NvBufSurfTransformError_Success ||
                enc_submit(enc, in, g_s[0].st.rec[f].feed) < 0) {
                ERR("compositor: encode frame %d", f);
                request_stop();
                break;
            }
            uint64_t t = now_us();
            pthread_mutex_lock(&g_mos.mu);
            for (int i = 0; i < n; i++) g_s[i].st.rec[f].out = t;
            g_mos.done[k] = 0;
            g_mos.slot_frame[k] = f + N_SLOTS;
            pthread_cond_broadcast(&g_mos.cv);
            pthread_mutex_unlock(&g_mos.mu);
        }
        if (g_stop) request_stop();      /* unblock workers waiting on a slot */
    }

    for (int i = 0; i < n; i++) {
        pthread_join(g_s[i].feeder, NULL);
        pthread_join(g_s[i].worker, NULL);
    }
    double wall = (now_us() - g_t0) / 1e6;

    ret = 0;
    for (int i = 0; i < n; i++) {
        if (g_s[i].failed || g_s[i].st.n != (uint32_t)g_opt.frames) {
            ERR("cam%d: processed %u/%d frames%s", i, g_s[i].st.n, g_opt.frames,
                g_s[i].failed ? " (failed)" : "");
            ret = 1;
        }
    }

    if (enc) {
        uint64_t bytes = 0;
        uint32_t frames = 0;
        if (enc_close(enc, &bytes, &frames) < 0) ret = 1;
        enc = NULL;
        LOG("encoded %u mosaic frames, %.2f MB -> %s", frames, bytes / 1e6, g_opt.output);
    }

    stream_stats_t st[MAX_STREAMS];
    for (int i = 0; i < n; i++) st[i] = g_s[i].st;
    char cfg[512];
    snprintf(cfg, sizeof(cfg),
             "{\"streams\": %d, \"engines\": \"%s,%s,%s,%s\", \"fps\": %.1f, \"frames\": %d, "
             "\"output\": %s, \"rt\": %d, \"pin\": %d, \"chunked\": %d}",
             n, g_opt.engines[0], g_opt.engines[1], g_opt.engines[2], g_opt.engines[3],
             g_opt.fps, g_opt.frames, g_opt.output ? "true" : "false", g_opt.rt, g_opt.pin, g_opt.chunked);
    stats_summary(st, n, wall, (uint32_t)g_opt.warmup, g_opt.json, cfg);
    if (g_opt.csv) stats_write_csv(g_opt.csv, st, n);

    for (int i = 0; i < n; i++) {
        fprintf(stderr, "cam%d detections:", i);
        for (int c = 0; c < g_n_labels; c++)
            fprintf(stderr, " %s=%u", g_labels[c], g_s[i].cls_count[c]);
        fprintf(stderr, "\n");
    }

out:
    if (enc) enc_close(enc, NULL, NULL);
    for (int i = 0; i < n; i++) {
        stream_t *s = &g_s[i];
        if (s->dec) dec_close(s->dec);
        cuda_unmap_surface(&s->infer_map);
        if (s->infer_surf) NvBufSurfaceDestroy(s->infer_surf);
        if (s->trt) trt_ctx_free(s->trt);
        if (s->cs) cudaStreamDestroy(s->cs);
        if (s->boxes) cudaFreeHost(s->boxes);
        stats_free(&s->st);
    }
    for (int k = 0; k < N_SLOTS; k++) {
        if (g_mos.surf[k]) {
            NvBufSurfaceUnMapEglImage(g_mos.surf[k], 0);
            NvBufSurfaceDestroy(g_mos.surf[k]);
        }
    }
    trt_engine_free(eng_gpu);
    trt_engine_free(eng_dla[0]);
    trt_engine_free(eng_dla[1]);
    h264_index_close(&g_ix);
    return ret;
}
