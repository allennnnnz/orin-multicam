/*
 * stats.c - see stats.h.
 */
#include "stats.h"
#include "common.h"

#include <stdlib.h>

int stats_init(stream_stats_t *s, const char *name, uint32_t cap)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->rec = calloc(cap, sizeof(*s->rec));
    s->cap = cap;
    return s->rec ? 0 : -1;
}

void stats_free(stream_stats_t *s)
{
    free(s->rec);
    memset(s, 0, sizeof(*s));
}

int stats_write_csv(const char *path, const stream_stats_t *s, int n_streams)
{
    FILE *f = fopen(path, "w");
    if (!f) { ERRNO("open %s", path); return -1; }
    fprintf(f, "stream,frame,feed_us,decode_ms,vic_ms,infer_ms,post_ms,detect_e2e_ms,overlay_ms,output_e2e_ms,ndet\n");
    for (int i = 0; i < n_streams; i++) {
        for (uint32_t k = 0; k < s[i].n; k++) {
            const frame_rec_t *r = &s[i].rec[k];
            fprintf(f, "%s,%u,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u\n", s[i].name, k,
                    (unsigned long long)r->feed,
                    (r->dec - r->feed) / 1e3, (r->vic - r->dec) / 1e3,
                    (r->inf - r->vic) / 1e3, (r->post - r->inf) / 1e3,
                    (r->post - r->feed) / 1e3,
                    r->ovl ? (r->ovl - r->post) / 1e3 : 0.0,
                    r->out ? (r->out - r->feed) / 1e3 : 0.0, r->ndet);
        }
    }
    fclose(f);
    return 0;
}

typedef struct { double p50, p99, max, mean; } dist_t;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static dist_t dist(double *v, uint32_t n)
{
    dist_t d = {0, 0, 0, 0};
    if (!n) return d;
    double sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += v[i];
    qsort(v, n, sizeof(*v), cmp_d);
    d.mean = sum / n;
    d.p50 = v[(n - 1) * 50 / 100];
    d.p99 = v[(n - 1) * 99 / 100];
    d.max = v[n - 1];
    return d;
}

enum { ST_DEC, ST_VIC, ST_INF, ST_POST, ST_DET_E2E, ST_OVL, ST_OUT_E2E, N_ST };
static const char *st_name[N_ST] = { "decode", "vic", "infer", "post", "detect_e2e", "overlay", "output_e2e" };

static double stage(const frame_rec_t *r, int st)
{
    switch (st) {
    case ST_DEC:     return (r->dec - r->feed) / 1e3;
    case ST_VIC:     return (r->vic - r->dec) / 1e3;
    case ST_INF:     return (r->inf - r->vic) / 1e3;
    case ST_POST:    return (r->post - r->inf) / 1e3;
    case ST_DET_E2E: return (r->post - r->feed) / 1e3;
    case ST_OVL:     return r->ovl ? (r->ovl - r->post) / 1e3 : -1;
    default:         return r->out ? (r->out - r->feed) / 1e3 : -1;
    }
}

void stats_summary(const stream_stats_t *s, int n_streams, double wall_s,
                   uint32_t warmup, const char *json_path, const char *config_json)
{
    FILE *j = json_path ? fopen(json_path, "w") : NULL;
    if (j) fprintf(j, "{\n  \"config\": %s,\n  \"wall_s\": %.3f,\n  \"streams\": [\n",
                   config_json ? config_json : "{}", wall_s);

    fprintf(stderr, "\n%-12s %6s %7s | %-24s | %-24s | %-24s\n", "stream", "frames", "fps",
            "infer ms p50/p99/max", "detect e2e p50/p99/max", "output e2e p50/p99/max");
    for (int i = 0; i < n_streams; i++) {
        uint32_t n = s[i].n > warmup ? s[i].n - warmup : 0;
        const frame_rec_t *r = s[i].rec + (s[i].n - n);
        double *v = malloc((n ? n : 1) * sizeof(double));
        dist_t d[N_ST];
        for (int st = 0; st < N_ST; st++) {
            uint32_t m = 0;
            for (uint32_t k = 0; k < n; k++) {
                double x = stage(&r[k], st);
                if (x >= 0) v[m++] = x;
            }
            d[st] = dist(v, m);
        }
        free(v);
        double fps = n > 1 ? (n - 1) * 1e6 / (double)(r[n - 1].post - r[0].post) : 0;
        fprintf(stderr, "%-12s %6u %7.1f | %6.2f %7.2f %8.2f | %6.2f %7.2f %8.2f | %6.2f %7.2f %8.2f\n",
                s[i].name, n, fps,
                d[ST_INF].p50, d[ST_INF].p99, d[ST_INF].max,
                d[ST_DET_E2E].p50, d[ST_DET_E2E].p99, d[ST_DET_E2E].max,
                d[ST_OUT_E2E].p50, d[ST_OUT_E2E].p99, d[ST_OUT_E2E].max);
        if (j) {
            fprintf(j, "    {\"name\": \"%s\", \"frames\": %u, \"fps\": %.2f", s[i].name, n, fps);
            for (int st = 0; st < N_ST; st++)
                fprintf(j, ", \"%s\": {\"mean\": %.3f, \"p50\": %.3f, \"p99\": %.3f, \"max\": %.3f}",
                        st_name[st], d[st].mean, d[st].p50, d[st].p99, d[st].max);
            fprintf(j, "}%s\n", i + 1 < n_streams ? "," : "");
        }
    }
    if (j) { fprintf(j, "  ]\n}\n"); fclose(j); }
}
